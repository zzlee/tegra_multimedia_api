#include "zznvcodec.h"
#include "NvVideoEncoder.h"
#include "ZzLog.h"

#include "NvUtils.h"
#include <errno.h>
#include <fstream>
#include <iostream>
#include <linux/videodev2.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <npp.h>
#include "NvBufSurface.h"

ZZ_INIT_LOG("zznvenc");

// #define DIRECT_OUTPUT
#define MAX_VIDEO_BUFFERS 2

struct Encoded_video_frame_t {
	int64_t TimeStamp;
	int DestBufferSize;
	unsigned char *DestBuffer;
};

struct zznvcodec_encoder_t {
	enum {
		STATE_READY,
		STATE_STARTED,
	} mState;

	NvVideoEncoder* mEncoder;

	int mWidth;
	int mHeight;
	zznvcodec_pixel_format_t mFormat;
	zznvcodec_encoder_on_video_packet_t mOnVideoPacket;

	Encoded_video_frame_t mEncodedFrames[MAX_VIDEO_BUFFERS];
	int mCurSaveIndex;
	int mCurGetIndex;
	intptr_t mOnVideoPacket_User;

	zznvcodec_codec_type_t mCodecType;
	int mBitRate;
	int mProfile;
	int mLevel;
	v4l2_mpeg_video_bitrate_mode mRateControl;
	int mIDRInterval;
	int mIFrameInterval;
	bool mLOWLATENCY;
	int mFrameRateNum;
	int mFrameRateDeno;

	zznvcodec_video_frame_t mYUY2VideoFrame; // NPP memory
	zznvcodec_video_frame_t mYV12VideoFrame; // NPP memory

	int mMaxPreloadBuffers;
	int mPreloadBuffersIndex;
	int mOutputPlaneFDs[32];

	zznvcodec_color_information_t mColorInformation;
	bool bExtColorFmt;

	explicit zznvcodec_encoder_t() {
		mState = STATE_READY;

		mEncoder = NULL;

		mWidth = 0;
		mHeight = 0;
		mFormat = ZZNVCODEC_PIXEL_FORMAT_UNKNOWN;
		mOnVideoPacket = NULL;
		mOnVideoPacket_User = 0;

		mCodecType = ZZNVCODEC_CODEC_TYPE_UNKNOWN;
		mBitRate = 16 * 1000000;
		mProfile = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
		mLevel = V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
		mRateControl = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
		mIDRInterval = 60;
		mIFrameInterval = 60;
		mLOWLATENCY = false;
		mFrameRateNum = 60;
		mFrameRateDeno = 1;

		mCurSaveIndex = 0;
		mCurGetIndex = 0;
		memset(mEncodedFrames, 0, sizeof(mEncodedFrames));
		memset(&mYUY2VideoFrame, 0, sizeof(mYUY2VideoFrame));
		memset(&mYV12VideoFrame, 0, sizeof(mYV12VideoFrame));

		mMaxPreloadBuffers = 2;
		mPreloadBuffersIndex = 0;
		memset(mOutputPlaneFDs, -1, sizeof(mOutputPlaneFDs));

		mColorInformation = ZZNVCODEC_COLOR_INFORMATION_601;
		bExtColorFmt = 0;
	}

	~zznvcodec_encoder_t() {
		if(mState != STATE_READY) {
			LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
		}
	}

	void SetVideoProperty(int nWidth, int nHeight, zznvcodec_pixel_format_t nFormat) {
		mWidth = nWidth;
		mHeight = nHeight;
		mFormat = nFormat;
	}

	void SetMiscProperty(int nProperty, intptr_t pValue) {
		switch(nProperty) {
		case ZZNVCODEC_PROP_CODEC_TYPE:
			mCodecType = *(zznvcodec_codec_type_t*)pValue;
			break;

		case ZZNVCODEC_PROP_BITRATE:
			mBitRate = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_PROFILE:
			mProfile = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_LEVEL:
			mLevel = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_RATECONTROL:
			mRateControl = (v4l2_mpeg_video_bitrate_mode)*(int*)pValue;
			break;

		case ZZNVCODEC_PROP_IDRINTERVAL:
			mIDRInterval = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_IFRAMEINTERVAL:
			mIFrameInterval = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_LOWLATENCY:
			mLOWLATENCY = *(int*)pValue;
			break;

		case ZZNVCODEC_PROP_FRAMERATE:
			mFrameRateNum = ((int*)pValue)[0];
			mFrameRateDeno = ((int*)pValue)[1];
			break;

		case ZZNVCODEC_PROP_COLORINFORMATION:
			mColorInformation = (zznvcodec_color_information_t)*(int*)pValue;
			break;

		case ZZNVCODEC_PROP_EXTCOLORFMT:
			bExtColorFmt = *(int*)pValue;
			break;

		default:
			LOGE("%s(%d): unexpected value, nProperty = %d", __FUNCTION__, __LINE__, nProperty);
			break;
		}
	}

	void RegisterCallbacks(zznvcodec_encoder_on_video_packet_t pCB, intptr_t pUser) {
		mOnVideoPacket = pCB;
		mOnVideoPacket_User = pUser;
	}

	int SetupOutputDMABuf(uint32_t num_buffers) {
		int ret = 0;
		NvBufSurf::NvCommonAllocateParams cParams;

#if 0
		LOGE("%s(%d): NvBufSurf::NvAllocate num_buffers=%d", __FUNCTION__, __LINE__, num_buffers);
#endif

		ret = mEncoder->output_plane.reqbufs(V4L2_MEMORY_DMABUF, num_buffers);
		if(ret) {
			LOGE("%s(%d): reqbufs failed for output plane V4L2_MEMORY_DMABUF", __FUNCTION__, __LINE__);
			return ret;
		}
		NvBufSurfaceColorFormat nColorFormat;
		switch(mFormat) {
		case ZZNVCODEC_PIXEL_FORMAT_NV12:	
			if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_SMPTE170M || (v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_DEFAULT)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_NV12 : nColorFormat = NVBUF_COLOR_FORMAT_NV12_ER;
			else if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_REC709)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_NV12_709 : nColorFormat = NVBUF_COLOR_FORMAT_NV12_709_ER;
			
			break;

		case ZZNVCODEC_PIXEL_FORMAT_YUV420P:
		case ZZNVCODEC_PIXEL_FORMAT_YUYV422:

			if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_SMPTE170M || (v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_DEFAULT)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_YUV420 : nColorFormat = NVBUF_COLOR_FORMAT_YUV420_ER;
			else if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_REC709)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_YUV420_709 : nColorFormat = NVBUF_COLOR_FORMAT_YUV420_709_ER;
			
			break;
		
		case ZZNVCODEC_PIXEL_FORMAT_NV24:

			//NV24 color information.
			if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_SMPTE170M || (v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_DEFAULT)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_NV24 : nColorFormat = NVBUF_COLOR_FORMAT_NV24_ER;
			else if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_REC709)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_NV24_709 : nColorFormat = NVBUF_COLOR_FORMAT_NV24_709_ER;	
			
			break;
		
		case ZZNVCODEC_PIXEL_FORMAT_YV24:	//Input is YV24(YVU), NV format is YUV444(YUV)

			if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_SMPTE170M || (v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_DEFAULT)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_YUV444 : nColorFormat = NVBUF_COLOR_FORMAT_YUV444_ER;
			else if ((v4l2_colorspace)mColorInformation == V4L2_COLORSPACE_REC709)
				!bExtColorFmt ? nColorFormat = NVBUF_COLOR_FORMAT_YUV444_709 : nColorFormat = NVBUF_COLOR_FORMAT_YUV444_709_ER;	
			
			break;			

		default:
			LOGE("%s(%d): unexpected value, mFormat=%d", __FUNCTION__, __LINE__, mFormat);
			break;
		}

		cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
		cParams.width = mWidth;
		cParams.height = mHeight;
		cParams.layout = NVBUF_LAYOUT_PITCH;
		cParams.colorFormat = nColorFormat;
		cParams.memtag = NvBufSurfaceTag_VIDEO_ENC;
		/* Create output plane fd for DMABUF io-mode */
		ret = NvBufSurf::NvAllocate(&cParams, mEncoder->output_plane.getNumBuffers(), mOutputPlaneFDs);

#if 0
		LOGE("%s(%d): NvBufSurf::NvAllocate mEncoder->output_plane.getNumBuffers=%d", __FUNCTION__, __LINE__, mEncoder->output_plane.getNumBuffers());
#endif

		if(ret < 0) {
			LOGE("%s(%d): NvBufSurf::NvAllocate failed, err=%d", __FUNCTION__, __LINE__, ret);
			return ret;
		}

		return ret;
	}

	static bool _EncoderCapturePlaneDQCallback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer, void *arg) {
		zznvcodec_encoder_t* pThis = (zznvcodec_encoder_t*)arg;

		return pThis->EncoderCapturePlaneDQCallback(v4l2_buf, buffer, shared_buffer);
	}

	bool EncoderCapturePlaneDQCallback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer) {
		if (v4l2_buf == NULL) {
			LOGE("%s(%d): Error while dequeing buffer from output plane", __FUNCTION__, __LINE__);
			return false;
		}

		if (buffer->planes[0].bytesused == 0) {
			LOGD("%s(%d): Got 0 size buffer in capture", __FUNCTION__, __LINE__);
			return false;
		}

		int flags = 0;
		v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
		if (mEncoder->getMetadata(v4l2_buf->index, enc_metadata) == 0) {
			if(enc_metadata.KeyFrame) {
				flags = 1;
			}
		}
#ifdef DIRECT_OUTPUT
		//LOGD("%s(%d): Real encoded frame", __FUNCTION__, __LINE__);
		mEncodedFrames[mCurSaveIndex].DestBuffer = (uint8_t*)buffer->planes[0].data;
		mEncodedFrames[mCurSaveIndex].DestBufferSize = buffer->planes[0].bytesused;
		mEncodedFrames[mCurSaveIndex].TimeStamp = v4l2_buf->timestamp.tv_sec * 1000000LL + v4l2_buf->timestamp.tv_usec;
		mCurSaveIndex = (mCurSaveIndex + 1) % MAX_VIDEO_BUFFERS;
#else
		if(mOnVideoPacket) {
			int64_t pts = v4l2_buf->timestamp.tv_sec * 1000000LL + v4l2_buf->timestamp.tv_usec;
			mOnVideoPacket((uint8_t*)buffer->planes[0].data, buffer->planes[0].bytesused, flags, pts, mOnVideoPacket_User);
		}
#endif
		if (mEncoder->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
			LOGE("%s(%d): Error while Qing buffer at capture plane", __FUNCTION__, __LINE__);
			return false;
		}

		return true;
	}

	int Start() {
		int ret;
		NppStatus status;

		if(mState != STATE_READY) {
			LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
			return 0;
		}

		LOGD("Start....");

		mEncoder = NvVideoEncoder::createVideoEncoder("enc0");
		if(! mEncoder) {
			LOGE("%s(%d): NvVideoEncoder::createVideoEncoder() failed", __FUNCTION__, __LINE__);
		}

		uint32_t codec_type = 0;
		uint8_t nEnable_MaxPerfMode = 1;
		switch(mCodecType) {
		case ZZNVCODEC_CODEC_TYPE_H264:
			codec_type = V4L2_PIX_FMT_H264;
			break;

		case ZZNVCODEC_CODEC_TYPE_H265:
			codec_type = V4L2_PIX_FMT_H265;
			mProfile = V4L2_MPEG_VIDEO_H265_PROFILE_MAIN;
			mLevel = V4L2_MPEG_VIDEO_H265_LEVEL_6_2_HIGH_TIER;
			break;

		case ZZNVCODEC_CODEC_TYPE_AV1:
			codec_type = V4L2_PIX_FMT_AV1;
			nEnable_MaxPerfMode = 1;
			break;

		default:
			LOGE("%s(%d): unexpected value, mCodecType=%d", __FUNCTION__, __LINE__, mCodecType);
			break;
		}

		ret = mEncoder->setCapturePlaneFormat(codec_type, mWidth, mHeight, 4 * 1024 * 1024);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setCapturePlaneFormat() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		uint32_t nV4L2PixFmt;
		bool bEnableLossless = false;
		uint8_t nChroma_format_idc = 0;

		switch(mFormat) {
		case ZZNVCODEC_PIXEL_FORMAT_YUV420P:
			nV4L2PixFmt = V4L2_PIX_FMT_YUV420M;
			break;

		case ZZNVCODEC_PIXEL_FORMAT_YUYV422: {
			nV4L2PixFmt = V4L2_PIX_FMT_YUV420M;
			{
				mYUY2VideoFrame.num_planes = 1;
				zznvcodec_video_plane_t& plane0 = mYUY2VideoFrame.planes[0];
				plane0.width = mWidth;
				plane0.height = mHeight;
				plane0.ptr = nppiMalloc_8u_C2(plane0.width, plane0.height, &plane0.stride);
			}

			{
				mYV12VideoFrame.num_planes = 3;
				zznvcodec_video_plane_t& plane0 = mYV12VideoFrame.planes[0];
				plane0.width = mWidth;
				plane0.height = mHeight;
				plane0.ptr = nppiMalloc_8u_C1(plane0.width, plane0.height, &plane0.stride);

				zznvcodec_video_plane_t& plane1 = mYV12VideoFrame.planes[1];
				plane1.width = mWidth / 2;
				plane1.height  = mHeight / 2;
				plane1.ptr = nppiMalloc_8u_C1(plane1.width, plane1.height, &plane1.stride);

				zznvcodec_video_plane_t& plane2 = mYV12VideoFrame.planes[2];
				plane2.width = mWidth / 2;
				plane2.height  = mHeight / 2;
				plane2.ptr = nppiMalloc_8u_C1(plane2.width, plane2.height, &plane2.stride);
			}
		}
			break;

		case ZZNVCODEC_PIXEL_FORMAT_NV12:
			nV4L2PixFmt = V4L2_PIX_FMT_NV12M;
			break;

		case ZZNVCODEC_PIXEL_FORMAT_NV24:
			nV4L2PixFmt = V4L2_PIX_FMT_NV24M;
			if (codec_type == V4L2_PIX_FMT_H264)
			{
				mProfile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
				bEnableLossless = true;
			}
			else if (codec_type == V4L2_PIX_FMT_H265)
				nChroma_format_idc = 3;
			break;

		case ZZNVCODEC_PIXEL_FORMAT_YV24:	//Input is YV24(YVU), NV format is YUV444(YUV)
			nV4L2PixFmt = V4L2_PIX_FMT_YUV444M;
			if (codec_type == V4L2_PIX_FMT_H264)
			{
				mProfile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
				bEnableLossless = true;
			}
			else if (codec_type == V4L2_PIX_FMT_H265)
				nChroma_format_idc = 3;
			break;
			
		default:
			nV4L2PixFmt = V4L2_PIX_FMT_YUV420M;
			LOGE("%s(%d): unexpected value, mFormat=%d", __FUNCTION__, __LINE__, mFormat);
			break;
		}
		
		ret = mEncoder->setOutputPlaneFormat(nV4L2PixFmt, mWidth, mHeight, mColorInformation);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setOutputPlaneFormat() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setBitrate(mBitRate);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setBitrate() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		if ((codec_type == V4L2_PIX_FMT_H264) || (codec_type == V4L2_PIX_FMT_H265))
		{
			ret = mEncoder->setProfile(mProfile);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setProfile() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}

			ret = mEncoder->setLevel(mLevel);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setLevel() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
		}

		// For H264 with NV24
		if (bEnableLossless == true)
		{
			ret = mEncoder->setLossless(bEnableLossless);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setLossless() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
		}

		// For H265 with NV24 or YUV444
		if (nChroma_format_idc == 3)
		{
			ret = mEncoder->setChromaFactorIDC(nChroma_format_idc);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setChromaFactorIDC() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
		}

		 /* Enable maximum performance mode by disabling internal DFS logic.
			NOTE: This enables encoder to run at max clocks */
		if (nEnable_MaxPerfMode)
		{
			ret = mEncoder->setMaxPerfMode(1);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setMaxPerfMode() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
		}

		ret = mEncoder->setRateControlMode(mRateControl);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setRateControlMode() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setIDRInterval(mIDRInterval);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setIDRInterval() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

        ret = mEncoder->setPocType(2);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setPocType() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		//Enable HW preset to high performance for all use modes.
		ret = mEncoder->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setHWPresetType() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

#if(1)
		ret = mEncoder->setInsertVuiEnabled(true);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setInsertVuiEnabled() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}
#endif

    	if (bExtColorFmt) {
			ret = mEncoder->setExtendedColorFormat(true);
			if(ret != 0) {
				LOGE("%s(%d): mEncoder->setExtendedColorFormat() failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
    	}

		ret = mEncoder->setNumBFrames(0);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setNumBFrames() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setIFrameInterval(mIFrameInterval);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setIFrameInterval() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setFrameRate(mFrameRateNum, mFrameRateDeno);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setFrameRate() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setInsertSpsPpsAtIdrEnabled(true);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setInsertSpsPpsAtIdrEnabled() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->setNumReferenceFrames(1);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setNumReferenceFrames() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = SetupOutputDMABuf(mMaxPreloadBuffers);
		if(ret != 0) {
			LOGE("%s(%d): SetupOutputDMABuf() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->capture_plane.setupPlane(V4L2_MEMORY_MMAP, mMaxPreloadBuffers, true, false);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->capture_plane.setupPlane(V4L2_MEMORY_MMAP) failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->subscribeEvent(V4L2_EVENT_EOS, 0, 0);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->subscribeEvent(V4L2_EVENT_EOS) failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		// output plane STREAMON
		ret = mEncoder->output_plane.setStreamStatus(true);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->output_plane.setStreamStatus() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		// capture plane STREAMON
		ret = mEncoder->capture_plane.setStreamStatus(true);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->capture_plane.setStreamStatus() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		mEncoder->capture_plane.setDQThreadCallback(_EncoderCapturePlaneDQCallback);
		mEncoder->capture_plane.startDQThread(this);

		ret = 1;
		// Enqueue all the empty capture plane buffers
		for (uint32_t i = 0; i < mEncoder->capture_plane.getNumBuffers(); i++)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];

			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

			v4l2_buf.index = i;
			v4l2_buf.m.planes = planes;

			ret = mEncoder->capture_plane.qBuffer(v4l2_buf, NULL);
			if (ret < 0) {
				LOGE("%s(%d): Error while queueing buffer at capture plane", __FUNCTION__, __LINE__);
				ret = 0;
				break;
			}
		}

		mState = STATE_STARTED;

		return ret;
	}

	void Stop() {
		int ret;

		if(mState != STATE_STARTED) {
			LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
			return;
		}

		LOGD("Stop....");

		ret = mEncoder->setEncoderCommand(V4L2_ENC_CMD_STOP, 1);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->setEncoderCommand(V4L2_ENC_CMD_STOP) failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->capture_plane.waitForDQThread(3000);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->capture_plane.waitForDQThread() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = mEncoder->output_plane.waitForDQThread(3000);
		if(ret != 0) {
			LOGE("%s(%d): mEncoder->output_plane.waitForDQThread() failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		for (uint32_t i = 0; i < mEncoder->output_plane.getNumBuffers(); i++) {
			ret = mEncoder->output_plane.unmapOutputBuffers(i, mOutputPlaneFDs[i]);
			if (ret < 0) {
				LOGE("%s(%d): mEncoder->output_plane.unmapOutputBuffers failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
			// LOGD("unmapOutputBuffers(%d)", i);

			ret = NvBufSurf::NvDestroy(mOutputPlaneFDs[i]);
			if(ret < 0) {
				LOGE("%s(%d): NvBufSurf::NvDestroy failed, err=%d", __FUNCTION__, __LINE__, ret);
			}
			// LOGD("NvBufSurf::NvDestroy(%d)", i);
		}
		memset(mOutputPlaneFDs, -1, sizeof(mOutputPlaneFDs));

#ifdef DIRECT_OUTPUT
		memset(mEncodedFrames, 0, sizeof(mEncodedFrames));
		mCurSaveIndex = 0;
		mCurGetIndex = 0;
#endif

		delete mEncoder;
		// LOGD("delete mEncoder");
		mEncoder = NULL;

		mPreloadBuffersIndex = 0;

		for(int i = 0;i < mYUY2VideoFrame.num_planes;++i) {
			nppiFree(mYUY2VideoFrame.planes[i].ptr);
		}
		memset(&mYUY2VideoFrame, 0, sizeof(mYUY2VideoFrame));

		for(int i = 0;i < mYV12VideoFrame.num_planes;++i) {
			nppiFree(mYV12VideoFrame.planes[i].ptr);
		}
		memset(&mYV12VideoFrame, 0, sizeof(mYV12VideoFrame));

		mState = STATE_READY;
	}

	void SetVideoUncompressionBuffer(zznvcodec_video_frame_t* pFrame, int64_t nTimestamp, bool bSetKeyFrame) {
		if ( pFrame != NULL) {
			int ret;
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			NvBuffer *buffer;
			NppStatus status;
			cudaError_t cudaError;

			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

			v4l2_buf.m.planes = planes;

			//if(mPreloadBuffersIndex == mEncoder->output_plane.getNumBuffers()) {			//test
			if( (mLOWLATENCY && (mPreloadBuffersIndex == 2)) || (!mLOWLATENCY && (mPreloadBuffersIndex == mEncoder->output_plane.getNumBuffers()))) {	//test
				// reused
				ret = mEncoder->output_plane.dqBuffer(v4l2_buf, &buffer, NULL, 10);
				if(ret < 0) {
					LOGE("%s(%d): Error DQing buffer at output plane", __FUNCTION__, __LINE__);
				}
			} else {
				// preload
				buffer = mEncoder->output_plane.getNthBuffer(mPreloadBuffersIndex);
				v4l2_buf.index = mPreloadBuffersIndex;
				v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
				v4l2_buf.memory = V4L2_MEMORY_DMABUF;
				ret = mEncoder->output_plane.mapOutputBuffers(v4l2_buf, mOutputPlaneFDs[mPreloadBuffersIndex]);
				if (ret < 0) {
					LOGE("%s(%d): Error while mapping buffer at output plane", __FUNCTION__, __LINE__);
				}

				mPreloadBuffersIndex++;
			}

			if(bSetKeyFrame)
				mEncoder->forceIDR();

			switch(mFormat) {
			case ZZNVCODEC_PIXEL_FORMAT_YUV420P: {
				if(pFrame->num_planes != 3) {
					LOGE("%s(%d): unexpected pFrame->num_planes = %d", __FUNCTION__, __LINE__, pFrame->num_planes);
					return;
				}

				for(int i = 0;i < 3;++i) {
					zznvcodec_video_plane_t& srcPlane = pFrame->planes[i];
					NvBuffer::NvBufferPlane &dstPlane = buffer->planes[i];

					cudaError = cudaMemcpy2D(dstPlane.data, dstPlane.fmt.stride, srcPlane.ptr, srcPlane.stride,
						srcPlane.width, srcPlane.height, cudaMemcpyHostToHost);
					if(cudaError != cudaSuccess) {
						LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", cudaError);
					}

					dstPlane.bytesused = dstPlane.fmt.stride * dstPlane.fmt.height;
				}
			}
				break;

			case ZZNVCODEC_PIXEL_FORMAT_YUYV422: {
				if(pFrame->num_planes != 1) {
					LOGE("%s(%d): unexpected pFrame->num_planes = %d", __FUNCTION__, __LINE__, pFrame->num_planes);
					return;
				}

				zznvcodec_video_plane_t& srcPlane = pFrame->planes[0];
				zznvcodec_video_plane_t& dstPlaneYUY2 = mYUY2VideoFrame.planes[0];
				cudaError = cudaMemcpy2D(dstPlaneYUY2.ptr, dstPlaneYUY2.stride,
					srcPlane.ptr, srcPlane.stride, srcPlane.width * 2, srcPlane.height, cudaMemcpyHostToDevice);
				if(cudaError != cudaSuccess) {
					LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", __FUNCTION__, __LINE__, cudaError);
				}

				Npp8u* pYV12[3] = { mYV12VideoFrame.planes[0].ptr, mYV12VideoFrame.planes[1].ptr, mYV12VideoFrame.planes[2].ptr };
				int nYV12Step[3] = { mYV12VideoFrame.planes[0].stride, mYV12VideoFrame.planes[1].stride, mYV12VideoFrame.planes[2].stride };
				NppiSize oSizeROI = { dstPlaneYUY2.width, dstPlaneYUY2.height };
				status = nppiYCbCr422ToYCbCr420_8u_C2P3R(dstPlaneYUY2.ptr, dstPlaneYUY2.stride, pYV12, nYV12Step, oSizeROI);
				if(status != 0) {
					LOGE("%s(%d): nppiYCbCr422ToYCbCr420_8u_C2P3R failed, status = %d", __FUNCTION__, __LINE__, status);
				}

				for(int i = 0;i < 3;++i) {
					zznvcodec_video_plane_t& srcPlane = mYV12VideoFrame.planes[i];
					NvBuffer::NvBufferPlane &dstPlane = buffer->planes[i];

					cudaError = cudaMemcpy2D(dstPlane.data, dstPlane.fmt.stride, srcPlane.ptr, srcPlane.stride,
						srcPlane.width, srcPlane.height, cudaMemcpyDeviceToHost);
					if(cudaError != cudaSuccess) {
						LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", __FUNCTION__, __LINE__, cudaError);
					}

					dstPlane.bytesused = dstPlane.fmt.stride * dstPlane.fmt.height;
				}
			}
				break;

			case ZZNVCODEC_PIXEL_FORMAT_NV12: {
				if(pFrame->num_planes != 2) {
					LOGE("%s(%d): unexpected pFrame->num_planes = %d", __FUNCTION__, __LINE__, pFrame->num_planes);
					return;
				}

				for(int i = 0;i < 2;++i) {
					zznvcodec_video_plane_t& srcPlane = pFrame->planes[i];
					NvBuffer::NvBufferPlane &dstPlane = buffer->planes[i];

					cudaError = cudaMemcpy2D(dstPlane.data, dstPlane.fmt.stride, srcPlane.ptr, srcPlane.stride,
						srcPlane.width, srcPlane.height, cudaMemcpyHostToHost);
					if(cudaError != cudaSuccess) {
						LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", cudaError);
					}

					dstPlane.bytesused = dstPlane.fmt.stride * dstPlane.fmt.height;
				}
			}
				break;

			case ZZNVCODEC_PIXEL_FORMAT_NV24: {
				if(pFrame->num_planes != 2) {
					LOGE("%s(%d): unexpected pFrame->num_planes = %d", __FUNCTION__, __LINE__, pFrame->num_planes);
					return;
				}

				for(int i = 0;i < 2;++i) {
					zznvcodec_video_plane_t& srcPlane = pFrame->planes[i];
					NvBuffer::NvBufferPlane &dstPlane = buffer->planes[i];

					cudaError = cudaMemcpy2D(dstPlane.data, dstPlane.fmt.stride, srcPlane.ptr, srcPlane.stride,
						srcPlane.width, srcPlane.height, cudaMemcpyHostToHost);
					if(cudaError != cudaSuccess) {
						LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", cudaError);
					}

					dstPlane.bytesused = dstPlane.fmt.stride * dstPlane.fmt.height;
				}
			}
				break;

			case ZZNVCODEC_PIXEL_FORMAT_YV24: {
				if(pFrame->num_planes != 3) {
					LOGE("%s(%d): unexpected pFrame->num_planes = %d", __FUNCTION__, __LINE__, pFrame->num_planes);
					return;
				}

				for(int i = 0;i < 3;++i) {
#if(0)
					zznvcodec_video_plane_t& srcPlane = pFrame->planes[i];
#else
					//Input is YV24(YVU), NV format is YUV444(YUV). So swap UV.
					zznvcodec_video_plane_t srcPlane;
					if(i == 0)
						srcPlane = pFrame->planes[i];
					else if(i == 1)
						srcPlane = pFrame->planes[2];
					else if(i == 2)
						srcPlane = pFrame->planes[1];
#endif
					NvBuffer::NvBufferPlane &dstPlane = buffer->planes[i];

					cudaError = cudaMemcpy2D(dstPlane.data, dstPlane.fmt.stride, srcPlane.ptr, srcPlane.stride,
						srcPlane.width, srcPlane.height, cudaMemcpyHostToHost);
					if(cudaError != cudaSuccess) {
						LOGE("%s(%d): cudaMemcpy2D failed, cudaError = %d", cudaError);
					}

					dstPlane.bytesused = dstPlane.fmt.stride * dstPlane.fmt.height;
				}
			}
				break;

			default:
				LOGE("%s(%d): unexpected value, mFormat=%d", __FUNCTION__, __LINE__, mFormat);
				break;
			}

			for (uint32_t j = 0;j < buffer->n_planes;j++) {
				NvBufSurface *nvbuf_surf = 0;
				ret = NvBufSurfaceFromFd(buffer->planes[j].fd, (void**)(&nvbuf_surf));
				if (ret < 0)
				{
					LOGE("%s(%d): NvBufSurfaceFromFd failed, err=%d", __FUNCTION__, __LINE__, ret);
					break;
				}

				ret = NvBufSurfaceSyncForDevice(nvbuf_surf, 0, j);
				if (ret < 0)
				{
					LOGE("%s(%d): NvBufSurfaceSyncForDevice failed, err=%d", __FUNCTION__, __LINE__, ret);
					break;
				}

				v4l2_buf.m.planes[j].bytesused = buffer->planes[j].bytesused;
			}

			v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
			v4l2_buf.timestamp.tv_sec = (int)(nTimestamp / 1000000);
			v4l2_buf.timestamp.tv_usec = (int)(nTimestamp % 1000000);

			ret = mEncoder->output_plane.qBuffer(v4l2_buf, NULL);
			if (ret < 0) {
				LOGE("%s(%d): Error while queueing buffer at output plane", __FUNCTION__, __LINE__);
			}
		}
	}

	void SetVideoUncompressionBuffer2(zznvcodec_video_frame_t* pFrame, int64_t nTimestamp, unsigned char *pDestBuffer, int *nDestBufferSize, int64_t *nDestTimestamp, bool bSetKeyFrame) {
		SetVideoUncompressionBuffer(pFrame, nTimestamp, bSetKeyFrame);

#ifdef DIRECT_OUTPUT
		if (mEncodedFrames[mCurGetIndex].DestBufferSize != 0) {
			//LOGD("%s(%d): buffer: index=%d DestBuffer:%p", __FUNCTION__, __LINE__, mCurGetIndex, mEncodedFrames[mCurGetIndex].DestBuffer);
			memcpy(pDestBuffer, mEncodedFrames[mCurGetIndex].DestBuffer, mEncodedFrames[mCurGetIndex].DestBufferSize);
			//pDestBuffer = mEncodedFrames[mCurGetIndex].DestBuffer;
			*nDestBufferSize = mEncodedFrames[mCurGetIndex].DestBufferSize;
			*nDestTimestamp = mEncodedFrames[mCurGetIndex].TimeStamp;
			mEncodedFrames[mCurGetIndex].DestBufferSize = 0;
			mCurGetIndex = (mCurGetIndex + 1) % MAX_VIDEO_BUFFERS;
		}
#endif
	}
};

zznvcodec_encoder_t* zznvcodec_encoder_new() {
	return new zznvcodec_encoder_t();
}

void zznvcodec_encoder_delete(zznvcodec_encoder_t* pThis) {
	delete pThis;
}

void zznvcodec_encoder_set_video_property(zznvcodec_encoder_t* pThis, int nWidth, int nHeight, zznvcodec_pixel_format_t nFormat) {
	pThis->SetVideoProperty(nWidth, nHeight, nFormat);
}

void zznvcodec_encoder_set_misc_property(zznvcodec_encoder_t* pThis, int nProperty, intptr_t pValue) {
	pThis->SetMiscProperty(nProperty, pValue);
}

void zznvcodec_encoder_register_callbacks(zznvcodec_encoder_t* pThis, zznvcodec_encoder_on_video_packet_t pCB, intptr_t pUser) {
	pThis->RegisterCallbacks(pCB, pUser);
}

int zznvcodec_encoder_start(zznvcodec_encoder_t* pThis) {
	return pThis->Start();
}

void zznvcodec_encoder_stop(zznvcodec_encoder_t* pThis) {
	return pThis->Stop();
}

void zznvcodec_encoder_set_video_uncompression_buffer(zznvcodec_encoder_t* pThis, zznvcodec_video_frame_t* pFrame, int64_t nTimestamp, bool bSetKeyFrame) {
	return pThis->SetVideoUncompressionBuffer(pFrame, nTimestamp, bSetKeyFrame);
}

void zznvcodec_encoder_set_video_uncompression_buffer2(zznvcodec_encoder_t* pThis, zznvcodec_video_frame_t* pFrame, int64_t nTimestamp, unsigned char *pDestBuffer, int *nDestBufferSize, int64_t *nDestTimestamp, bool bSetKeyFrame) {
	return pThis->SetVideoUncompressionBuffer2(pFrame, nTimestamp, pDestBuffer, nDestBufferSize, nDestTimestamp, bSetKeyFrame);
}
