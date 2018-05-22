////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2018 Nicholas Frechette
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "AnimCompress_ACL.h"
#include "Animation/AnimEncodingRegistry.h"

#if WITH_EDITOR
#include "ACLImpl.h"

#include <acl/algorithm/uniformly_sampled/encoder.h>
#include <acl/algorithm/uniformly_sampled/decoder.h>
#include <acl/compression/skeleton.h>
#include <acl/compression/animation_clip.h>
#include <acl/compression/skeleton_error_metric.h>
#include <acl/compression/utils.h>

#include <sjson/writer.h>
#include <acl/io/clip_writer.h>
#endif	// WITH_EDITOR

UAnimCompress_ACL::UAnimCompress_ACL(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Description = TEXT("ACL");
	bNeedsSkeleton = true;

	RotationFormat = ACLRotationFormat::QuatDropW_Variable;
	TranslationFormat = ACLVectorFormat::Vector3_Variable;
	ScaleFormat = ACLVectorFormat::Vector3_Variable;

	bClipRangeReduceRotations = true;
	bClipRangeReduceTranslations = true;
	bClipRangeReduceScales = true;

	bEnableSegmenting = true;
	bSegmentRangeReduceRotations = true;
	bSegmentRangeReduceTranslations = true;
	bSegmentRangeReduceScales = true;
	IdealNumKeyFramesPerSegment = 16;
	MaxNumKeyFramesPerSegment = 31;
}

#if WITH_EDITOR
static acl::RotationFormat8 GetRotationFormat(ACLRotationFormat Format)
{
	switch (Format)
	{
	default:
	case ACLRotationFormat::Quat_128:			return acl::RotationFormat8::Quat_128;
	case ACLRotationFormat::QuatDropW_96:		return acl::RotationFormat8::QuatDropW_96;
	case ACLRotationFormat::QuatDropW_Variable:	return acl::RotationFormat8::QuatDropW_Variable;
	}
}

static acl::VectorFormat8 GetVectorFormat(ACLVectorFormat Format)
{
	switch (Format)
	{
	default:
	case ACLVectorFormat::Vector3_96:			return acl::VectorFormat8::Vector3_96;
	case ACLVectorFormat::Vector3_Variable:		return acl::VectorFormat8::Vector3_Variable;
	}
}

static acl::RigidSkeleton* BuildACLSkeleton(ACLAllocator& AllocatorImpl, const UAnimSequence& AnimSeq, const TArray<FBoneData>& BoneData)
{
	using namespace acl;

	const int32 NumBones = BoneData.Num();

	TArray<RigidBone> ACLSkeletonBones;
	ACLSkeletonBones.Empty(NumBones);
	ACLSkeletonBones.AddDefaulted(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FBoneData& UE4Bone = BoneData[BoneIndex];
		RigidBone& ACLBone = ACLSkeletonBones[BoneIndex];
		ACLBone.name = String(AllocatorImpl, TCHAR_TO_ANSI(*UE4Bone.Name.ToString()));
		ACLBone.bind_transform = transform_cast(transform_set(QuatCast(UE4Bone.Orientation), VectorCast(UE4Bone.Position), vector_set(1.0f)));

		// We use a higher virtual vertex distance when bones have a socket attached or are keyed end effectors (IK, hand, camera, etc)
		// We use 100cm instead of 3cm. UE 4 usually uses 50cm (END_EFFECTOR_SOCKET_DUMMY_BONE_SIZE) but
		// we use a higher value anyway due to the fact that ACL has no error compensation and it is more aggressive.
		ACLBone.vertex_distance = (UE4Bone.bHasSocket || UE4Bone.bKeyEndEffector) ? 100.0f : 3.0f;

		const int32 ParentBoneIndex = UE4Bone.GetParent();
		ACLBone.parent_index = ParentBoneIndex >= 0 ? safe_static_cast<uint16_t>(ParentBoneIndex) : acl::k_invalid_bone_index;
	}

	return new RigidSkeleton(AllocatorImpl, ACLSkeletonBones.GetData(), NumBones);
}

static int32 FindAnimationTrackIndex(const UAnimSequence& AnimSeq, int32 BoneIndex)
{
	const TArray<FTrackToSkeletonMap>& TrackToSkelMap = AnimSeq.GetRawTrackToSkeletonMapTable();
	if (BoneIndex != INDEX_NONE)
	{
		for (int32 TrackIndex = 0; TrackIndex < TrackToSkelMap.Num(); ++TrackIndex)
		{
			const FTrackToSkeletonMap& TrackToSkeleton = TrackToSkelMap[TrackIndex];
			if (TrackToSkeleton.BoneTreeIndex == BoneIndex)
				return TrackIndex;
		}
	}

	return INDEX_NONE;
}

static acl::AnimationClip* BuildACLClip(ACLAllocator& AllocatorImpl, const UAnimSequence* AnimSeq, const acl::RigidSkeleton& ACLSkeleton, int32 RefFrameIndex, bool IsAdditive)
{
	using namespace acl;

	// Additive animations have 0,0,0 scale as the default since we add it
	const FVector UE4DefaultScale(IsAdditive ? 0.0f : 1.0f);
	const Vector4_64 ACLDefaultScale = vector_set(IsAdditive ? 0.0 : 1.0);

	if (AnimSeq != nullptr)
	{
		const TArray<FRawAnimSequenceTrack>& RawTracks = AnimSeq->GetRawAnimationData();
		const uint32 NumSamples = RefFrameIndex >= 0 ? 1 : AnimSeq->NumFrames;
		const uint32 SampleRate = RefFrameIndex >= 0 ? 30 : FMath::TruncToInt(((AnimSeq->NumFrames - 1) / AnimSeq->SequenceLength) + 0.5f);
		const uint32 FirstSampleIndex = RefFrameIndex >= 0 ? FMath::Min(RefFrameIndex, AnimSeq->NumFrames - 1) : 0;
		const String ClipName(AllocatorImpl, TCHAR_TO_ANSI(*AnimSeq->GetPathName()));

		AnimationClip* ACLClip = new AnimationClip(AllocatorImpl, ACLSkeleton, NumSamples, SampleRate, ClipName);

		AnimatedBone* ACLBones = ACLClip->get_bones();
		const uint16 NumBones = ACLSkeleton.get_num_bones();
		for (uint16 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const int32 TrackIndex = FindAnimationTrackIndex(*AnimSeq, BoneIndex);

			AnimatedBone& ACLBone = ACLBones[BoneIndex];

			// We output bone data in UE4 track order. If a track isn't present, we will use the bind pose and strip it from the
			// compressed stream.
			ACLBone.output_index = TrackIndex >= 0 ? TrackIndex : -1;

			if (TrackIndex >= 0)
			{
				// We have a track for this bone, use it
				const FRawAnimSequenceTrack& RawTrack = RawTracks[TrackIndex];

				for (uint32 SampleIndex = FirstSampleIndex; SampleIndex < NumSamples; ++SampleIndex)
				{
					const FQuat& RotationSample = RawTrack.RotKeys.Num() == 1 ? RawTrack.RotKeys[0] : RawTrack.RotKeys[SampleIndex];
					ACLBone.rotation_track.set_sample(SampleIndex, quat_cast(QuatCast(RotationSample)));

					const FVector& TranslationSample = RawTrack.PosKeys.Num() == 1 ? RawTrack.PosKeys[0] : RawTrack.PosKeys[SampleIndex];
					ACLBone.translation_track.set_sample(SampleIndex, vector_cast(VectorCast(TranslationSample)));

					const FVector& ScaleSample = RawTrack.ScaleKeys.Num() == 0 ? UE4DefaultScale : (RawTrack.ScaleKeys.Num() == 1 ? RawTrack.ScaleKeys[0] : RawTrack.ScaleKeys[SampleIndex]);
					ACLBone.scale_track.set_sample(SampleIndex, vector_cast(VectorCast(ScaleSample)));
				}
			}
			else
			{
				// No track data for this bone, it must be new. Use the bind pose instead
				const RigidBone& ACLRigidBone = ACLSkeleton.get_bone(BoneIndex);

				for (uint32 SampleIndex = FirstSampleIndex; SampleIndex < NumSamples; ++SampleIndex)
				{
					ACLBone.rotation_track.set_sample(SampleIndex, ACLRigidBone.bind_transform.rotation);
					ACLBone.translation_track.set_sample(SampleIndex, ACLRigidBone.bind_transform.translation);
					ACLBone.scale_track.set_sample(SampleIndex, ACLDefaultScale);
				}
			}
		}

		return ACLClip;
	}
	else
	{
		// No animation sequence provided, use the bind pose instead
		check(!IsAdditive);

		const uint16 NumBones = ACLSkeleton.get_num_bones();
		const uint32 NumSamples = 1;
		const uint32 SampleRate = 30;
		const String ClipName(AllocatorImpl, "Bind Pose");

		AnimationClip* ACLClip = new AnimationClip(AllocatorImpl, ACLSkeleton, NumSamples, SampleRate, ClipName);

		AnimatedBone* ACLBones = ACLClip->get_bones();
		for (uint16 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			// Get the bind transform and make sure it has no scale
			const RigidBone& skel_bone = ACLSkeleton.get_bone(BoneIndex);
			const Transform_64 bind_transform = transform_set(skel_bone.bind_transform.rotation, skel_bone.bind_transform.translation, ACLDefaultScale);

			ACLBones[BoneIndex].rotation_track.set_sample(0, bind_transform.rotation);
			ACLBones[BoneIndex].translation_track.set_sample(0, bind_transform.translation);
			ACLBones[BoneIndex].scale_track.set_sample(0, bind_transform.scale);
		}

		return ACLClip;
	}
}

static bool IsUsingDefaultCompressionSettings(const acl::CompressionSettings& Settings)
{
	using namespace acl;

	return Settings.rotation_format == RotationFormat8::QuatDropW_Variable
		&& Settings.translation_format == VectorFormat8::Vector3_Variable
		&& Settings.scale_format == VectorFormat8::Vector3_Variable
		&& are_all_enum_flags_set(Settings.range_reduction, RangeReductionFlags8::AllTracks)
		&& Settings.segmenting.enabled;
}

void UAnimCompress_ACL::DoReduction(UAnimSequence* AnimSeq, const TArray<FBoneData>& BoneData)
{
	using namespace acl;

	AnimSeq->KeyEncodingFormat = AKF_MAX;
	FAnimEncodingRegistry::Get().SetInterfaceLinks(*AnimSeq);

	AnimSeq->CompressionScheme = static_cast<UAnimCompress*>(StaticDuplicateObject(this, AnimSeq));

	ACLAllocator AllocatorImpl;

	RigidSkeleton* ACLSkeleton = BuildACLSkeleton(AllocatorImpl, *AnimSeq, BoneData);
	AnimationClip* ACLClip = BuildACLClip(AllocatorImpl, AnimSeq, *ACLSkeleton, -1, AnimSeq->IsValidAdditive());
	AnimationClip* ACLBaseClip = nullptr;

	if (AnimSeq->IsValidAdditive())
	{
		if (AnimSeq->RefPoseType == ABPT_RefPose)
			ACLBaseClip = BuildACLClip(AllocatorImpl, nullptr, *ACLSkeleton, -1, false);
		else if (AnimSeq->RefPoseType == ABPT_AnimScaled)
			ACLBaseClip = BuildACLClip(AllocatorImpl, AnimSeq->RefPoseSeq, *ACLSkeleton, -1, false);
		else if (AnimSeq->RefPoseType == ABPT_AnimFrame)
			ACLBaseClip = BuildACLClip(AllocatorImpl, AnimSeq->RefPoseSeq, *ACLSkeleton, AnimSeq->RefFrameIndex, false);

		if (ACLBaseClip != nullptr)
			ACLClip->set_additive_base(ACLBaseClip, AdditiveClipFormat8::Additive1);
	}

	OutputStats Stats;

	CompressionSettings Settings;
	Settings.rotation_format = GetRotationFormat(RotationFormat);
	Settings.translation_format = GetVectorFormat(TranslationFormat);
	Settings.scale_format = GetVectorFormat(ScaleFormat);
	Settings.range_reduction |= bClipRangeReduceRotations ? RangeReductionFlags8::Rotations : RangeReductionFlags8::None;
	Settings.range_reduction |= bClipRangeReduceTranslations ? RangeReductionFlags8::Translations : RangeReductionFlags8::None;
	Settings.range_reduction |= bClipRangeReduceScales ? RangeReductionFlags8::Scales : RangeReductionFlags8::None;
	Settings.segmenting.enabled = bEnableSegmenting != 0;
	Settings.segmenting.ideal_num_samples = IdealNumKeyFramesPerSegment;
	Settings.segmenting.max_num_samples = MaxNumKeyFramesPerSegment;
	Settings.segmenting.range_reduction |= bSegmentRangeReduceRotations ? RangeReductionFlags8::Rotations : RangeReductionFlags8::None;
	Settings.segmenting.range_reduction |= bSegmentRangeReduceTranslations ? RangeReductionFlags8::Translations : RangeReductionFlags8::None;
	Settings.segmenting.range_reduction |= bSegmentRangeReduceScales ? RangeReductionFlags8::Scales : RangeReductionFlags8::None;

	TransformErrorMetric DefaultErrorMetric;
	AdditiveTransformErrorMetric<AdditiveClipFormat8::Additive1> AdditiveErrorMetric;
	if (ACLBaseClip != nullptr)
		Settings.error_metric = &AdditiveErrorMetric;
	else
		Settings.error_metric = &DefaultErrorMetric;

	Settings.error_threshold = 0.01f;

	const bool IsDecompressionFastPath = IsUsingDefaultCompressionSettings(Settings);

	static volatile bool DumpClip = false;
	if (DumpClip)
		write_acl_clip(*ACLSkeleton, *ACLClip, AlgorithmType8::UniformlySampled, Settings, "D:\\acl_clip.acl.sjson");

	CompressedClip* CompressedClipData = nullptr;
	ErrorResult CompressionResult = uniformly_sampled::compress_clip(AllocatorImpl, *ACLClip, Settings, CompressedClipData, Stats);
	if (CompressionResult.empty())
	{
		check(CompressedClipData != nullptr);
		check(CompressedClipData->is_valid(true).empty());

		const uint32 CompressedClipDataSize = CompressedClipData->get_size();

		AnimSeq->CompressedByteStream.Empty(CompressedClipDataSize + 1);
		AnimSeq->CompressedByteStream.AddUninitialized(CompressedClipDataSize + 1);
		memcpy(AnimSeq->CompressedByteStream.GetData(), CompressedClipData, CompressedClipDataSize);
		AnimSeq->CompressedByteStream[CompressedClipDataSize] = IsDecompressionFastPath ? 1 : 0;

		uniformly_sampled::DebugDecompressionSettings DecompressionSettings;
		uniformly_sampled::DecompressionContext<uniformly_sampled::DebugDecompressionSettings> Context(DecompressionSettings);
		Context.initialize(*CompressedClipData);
		const BoneError bone_error = calculate_compressed_clip_error(AllocatorImpl, *ACLClip, Settings, Context);

		UE_LOG(LogAnimation, Warning, TEXT("ACL Animation raw size: %u bytes"), ACLClip->get_raw_size());
		UE_LOG(LogAnimation, Warning, TEXT("ACL Animation error: %.4f cm (bone %u @ %.3f)"), bone_error.error, bone_error.index, bone_error.sample_time);

		AllocatorImpl.deallocate(CompressedClipData, CompressedClipData->get_size());
	}
	else
	{
		AnimSeq->CompressedByteStream.Empty();
		UE_LOG(LogAnimation, Error, TEXT("ACL failed to compress clip: %s"), ANSI_TO_TCHAR(CompressionResult.c_str()));
	}

	delete ACLBaseClip;
	delete ACLClip;
	delete ACLSkeleton;
}

void UAnimCompress_ACL::PopulateDDCKey(FArchive& Ar)
{
	Super::PopulateDDCKey(Ar);

	uint8 Flags =	MakeBitForFlag(bClipRangeReduceRotations, 0) +
					MakeBitForFlag(bClipRangeReduceTranslations, 1) +
					MakeBitForFlag(bClipRangeReduceScales, 2) +
					MakeBitForFlag(bEnableSegmenting, 3) +
					MakeBitForFlag(bSegmentRangeReduceRotations, 4) +
					MakeBitForFlag(bSegmentRangeReduceTranslations, 5) +
					MakeBitForFlag(bSegmentRangeReduceScales, 6);

	uint32 ForceRebuildVersion = 0;
	uint16 AlgorithmVersion = acl::get_algorithm_version(acl::AlgorithmType8::UniformlySampled);

	Ar	<< RotationFormat << TranslationFormat << ScaleFormat
		<< Flags
		<< IdealNumKeyFramesPerSegment << MaxNumKeyFramesPerSegment
		<< ForceRebuildVersion << AlgorithmVersion;
}
#endif // WITH_EDITOR