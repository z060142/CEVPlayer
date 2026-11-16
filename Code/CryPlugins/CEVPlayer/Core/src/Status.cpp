#include "CEVCore/Status.h"

namespace cev
{
	const char* StatusToString(Status status)
	{
		switch (status)
		{
		case Status::Ok: return "Ok";
		case Status::InvalidArgument: return "InvalidArgument";
		case Status::InvalidState: return "InvalidState";
		case Status::NotReady: return "NotReady";
		case Status::OutOfMemory: return "OutOfMemory";
		case Status::IoError: return "IoError";
		case Status::OutOfRange: return "OutOfRange";
		case Status::InvalidData: return "InvalidData";
		case Status::Unsupported: return "Unsupported";
		case Status::DecodeError: return "DecodeError";
		case Status::Cancelled: return "Cancelled";
		case Status::LoadFailed: return "LoadFailed";
		}
		return "Unknown";
	}

	const char* StatusDetailToString(StatusDetail detail)
	{
		switch (detail)
		{
		case StatusDetail::None: return "None";
		case StatusDetail::EbmlHeader: return "EbmlHeader";
		case StatusDetail::SegmentParse: return "SegmentParse";
		case StatusDetail::TracksParse: return "TracksParse";
		case StatusDetail::ClusterScan: return "ClusterScan";
		case StatusDetail::NoSupportedTrack: return "NoSupportedTrack";
		case StatusDetail::ContentEncoding: return "ContentEncoding";
		case StatusDetail::BadCodecPrivate: return "BadCodecPrivate";
		case StatusDetail::VideoDecoderInit: return "VideoDecoderInit";
		case StatusDetail::AudioDecoderInit: return "AudioDecoderInit";
		case StatusDetail::FirstFrame: return "FirstFrame";
		case StatusDetail::RingInit: return "RingInit";
		case StatusDetail::PoolRegister: return "PoolRegister";
		case StatusDetail::StreamChanged: return "StreamChanged";
		case StatusDetail::InternalError: return "InternalError";
		}
		return "Unknown";
	}
}
