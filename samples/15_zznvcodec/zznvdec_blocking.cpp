#include "zznvdec.h"
#include "ZzLog.h"

#include "NvVideoDecoder.h"
#include "NvUtils.h"
#include "NvBufSurface.h"

ZZ_INIT_LOG("zznvdec_blocking");

#define _countof(x) (sizeof(x)/sizeof(x[0]))
#define CHUNK_SIZE 4000000
#define MAX_BUFFERS 64

// #define DIRECT_OUTPUT
#define MAX_VIDEO_BUFFERS 12

#define IS_NAL_UNIT_START(buffer_ptr) \
(!buffer_ptr[0] && !buffer_ptr[1] && \
!buffer_ptr[2] && (buffer_ptr[3] == 1))

#define IS_NAL_UNIT_START1(buffer_ptr) \
(!buffer_ptr[0] && !buffer_ptr[1] && \
(buffer_ptr[2] == 1))

#define GET_H265_NAL_UNIT_TYPE(buffer_ptr, offset) ((buffer_ptr[offset] & 0x7E) >> 1)
#define GET_H265_FIRST_SLICE_SEGMENT_IN_PIC_FLAG(buffer_ptr, offset) ((buffer_ptr[offset] & 0x80))

// slice
#define HEVC_NUT_TRAIL_N  0
#define HEVC_NUT_RASL_R  9
#define HEVC_NUT_BLA_W_LP  16
#define HEVC_NUT_CRA_NUT  21

struct Decoded_video_frame_t {
	int64_t TimeStamp;
	int64_t DestBufferSize;
	zznvcodec_video_frame_t *DestBuffer;
};

struct zznvcodec_decoder_blocking : public zznvcodec_decoder_t {
	typedef zznvcodec_decoder_blocking self_t;
	typedef zznvcodec_decoder_t super_t;

	explicit zznvcodec_decoder_blocking();
	virtual ~zznvcodec_decoder_blocking();

	virtual int Start();
	virtual void Stop();
	virtual void SetVideoCompressionBuffer(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp);
	virtual void SetVideoCompressionBuffer2(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp, unsigned char *pDestBuffer, int64_t *nDestBufferSize, int64_t *nDestTimestamp);
	virtual int GetVideoCompressionBuffer(zznvcodec_video_frame_t** ppFrame, int64_t* pTimestamp);

	enum {
		STATE_READY,
		STATE_STARTED,
	} mState;

	NvVideoDecoder* mDecoder;
	pthread_t mDecoderThread;
	int mDMABufFDs[MAX_BUFFERS];
	int mNumCapBuffers;
	int mVideoDMAFDs[MAX_VIDEO_BUFFERS];
	zznvcodec_video_frame_t mVideoFrames[MAX_VIDEO_BUFFERS];
	int mCurVideoOutputIndex;
	int mCurVideoDMAFDIndex;
	int mFormatWidth;
	int mFormatHeight;
	volatile int mGotEOS;
	int mMaxPreloadBuffers;
	int mPreloadBuffersIndex;
	NvBufSurfaceColorFormat mBufferColorFormat;
	int mV4L2PixFmt;

#ifdef DIRECT_OUTPUT
	Decoded_video_frame_t mDecodedFrames[MAX_VIDEO_BUFFERS];
	int64_t mDecodedFrameSize;
#endif

	void FreeBuffers();
	void EnqueuePacket(unsigned char* pBuffer, int nSize, int64_t nTimestamp);

	static void* _DecodeMain(void* arg);
	void* DecoderMain();

	void QueryAndSetCapture();
};

zznvcodec_decoder_t* NewDecoder_blocking() {
	return new zznvcodec_decoder_blocking();
}

zznvcodec_decoder_blocking::zznvcodec_decoder_blocking() {
	mState = STATE_READY;

	mDecoder = NULL;
	mDecoderThread = (pthread_t)NULL;
	memset(mDMABufFDs, -1, sizeof(mDMABufFDs));
	mNumCapBuffers = 0;
	memset(mVideoDMAFDs, -1, sizeof(mVideoDMAFDs));
	memset(mVideoFrames, 0, sizeof(mVideoFrames));
	mCurVideoOutputIndex = 0;
	mCurVideoDMAFDIndex = 0;
	mFormatWidth = 0;
	mFormatHeight = 0;
	mGotEOS = 0;
	mMaxPreloadBuffers = MAX_VIDEO_BUFFERS;
	mPreloadBuffersIndex = 0;
	mBufferColorFormat = NVBUF_COLOR_FORMAT_INVALID;
	mV4L2PixFmt = 0;

#ifdef DIRECT_OUTPUT
	memset(mDecodedFrames, 0, sizeof(mDecodedFrames));
	mDecodedFrameSize = 0;
#endif
}

zznvcodec_decoder_blocking::~zznvcodec_decoder_blocking() {
	if(mState != STATE_READY) {
		LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
	}
}

int zznvcodec_decoder_blocking::Start() {
	int ret;

	if(mState != STATE_READY) {
		LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
		return 0;
	}

	LOGD("Start decoder...");

	switch(mFormat) {
	case ZZNVCODEC_PIXEL_FORMAT_NV12:
		mBufferColorFormat = NVBUF_COLOR_FORMAT_NV12;
#if DIRECT_OUTPUT
		mDecodedFrameSize = mWidth * mHeight * 3 >> 1;
#endif
		break;

	case ZZNVCODEC_PIXEL_FORMAT_NV24:
		mBufferColorFormat = NVBUF_COLOR_FORMAT_NV24;
#if DIRECT_OUTPUT
		mDecodedFrameSize = mWidth * mHeight * 3;
#endif
		break;

	case ZZNVCODEC_PIXEL_FORMAT_YUV420P:
		mBufferColorFormat = NVBUF_COLOR_FORMAT_YUV420;
#if DIRECT_OUTPUT
		mDecodedFrameSize = mWidth * mHeight * 3 >> 1;
#endif
		break;

	default:
		LOGE("%s(%d): unexpected value, mFormat=%d", __FUNCTION__, __LINE__, mFormat);
		break;
	}

	switch(mCodecType) {
	case ZZNVCODEC_CODEC_TYPE_H264:
		mV4L2PixFmt = V4L2_PIX_FMT_H264;
		break;

	case ZZNVCODEC_CODEC_TYPE_H265:
		mV4L2PixFmt = V4L2_PIX_FMT_H265;
		break;

	case ZZNVCODEC_CODEC_TYPE_AV1:
		mV4L2PixFmt = V4L2_PIX_FMT_AV1;
		break;

	default:
		LOGE("%s(%d): unexpected value, mCodecType=%d", __FUNCTION__, __LINE__, mCodecType);
		break;
	}

	mDecoder = NvVideoDecoder::createVideoDecoder("dec0");
	if(! mDecoder) {
		LOGE("%s(%d): NvVideoDecoder::createVideoDecoder failed", __FUNCTION__, __LINE__);
	}

	ret = mDecoder->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
	if(ret) {
		LOGE("%s(%d): subscribeEvent failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = mDecoder->setOutputPlaneFormat(mV4L2PixFmt, CHUNK_SIZE);
	if(ret) {
		LOGE("%s(%d): setOutputPlaneFormat failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = mDecoder->setFrameInputMode(0); // 0 --> NALu-based, 1 --> Chunk-based
	if(ret) {
		LOGE("%s(%d): setFrameInputMode failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	
	if(mLOWLATENCY)
	{
		ret = mDecoder->disableDPB();
		if(ret) {
			LOGE("%s(%d): disableDPB failed, err=%d", __FUNCTION__, __LINE__, ret);
		}
	}

	//if (mFormat == ZZNVCODEC_PIXEL_FORMAT_NV24)
	{
		ret = mDecoder->setMaxPerfMode(1);
		if(ret) {
			LOGE("%s(%d): setMaxPerfMode failed, err=%d", __FUNCTION__, __LINE__, ret);
		}
	}

	LOGI("%s(%u)MAX_VIDEO_BUFFERS=%d H265 Slice detect", __FUNCTION__, __LINE__, MAX_VIDEO_BUFFERS);
	ret = mDecoder->output_plane.setupPlane(V4L2_MEMORY_MMAP, mMaxPreloadBuffers, true, false);
	if(ret) {
		LOGE("%s(%d): setupPlane failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = mDecoder->output_plane.setStreamStatus(true);
	if(ret) {
		LOGE("%s(%d): setStreamStatus failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = pthread_create(&mDecoderThread, NULL, _DecodeMain, this);
	if(ret) {
		ret = errno;
		LOGE("%s(%d): pthread_create failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	mState = STATE_STARTED;

	LOGD("Start decoder... DONE");

	return 1;
}

void zznvcodec_decoder_blocking::FreeBuffers() {
	int ret;

	for(int i = 0;i < _countof(mDMABufFDs);i++) {
		if(mDMABufFDs[i] == -1)
			continue;

		ret = NvBufSurf::NvDestroy(mDMABufFDs[i]);
		if(ret < 0) {
			LOGE("%s(%d): NvBufSurf::NvDestroy failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		mDMABufFDs[i] = -1;
	}
	for(int i = 0;i < _countof(mVideoDMAFDs);i++) {
		if(mVideoDMAFDs[i] == -1)
			continue;

		NvBufSurface *nvbuf_surf = NULL;
		ret = NvBufSurfaceFromFd(mVideoDMAFDs[i], (void**)(&nvbuf_surf));
		if (ret != 0) {
			LOGE("%s(%d): NvBufSurfaceFromFd failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = NvBufSurfaceUnMap(nvbuf_surf, 0, -1);
		if (ret != 0) {
			LOGE("%s(%d): NvBufSurfaceUnMap failed, err=%d", __FUNCTION__, __LINE__, i, ret);
		}
	}
#ifdef DIRECT_OUTPUT
	memset(mDecodedFrames, 0, sizeof(mDecodedFrames));
#endif

	memset(mVideoFrames, 0, sizeof(mVideoFrames));
	for(int i = 0;i < _countof(mVideoDMAFDs);i++) {
		if(mVideoDMAFDs[i] == -1)
			continue;

		ret = NvBufSurf::NvDestroy(mVideoDMAFDs[i]);
		if(ret < 0) {
			LOGE("%s(%d): NvBufSurf::NvDestroy failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		mVideoDMAFDs[i] = -1;
	}
}

void zznvcodec_decoder_blocking::Stop() {
	int ret;

	if(mState != STATE_STARTED) {
		LOGE("%s(%d): unexpected value, mState=%d", __FUNCTION__, __LINE__, mState);
		return;
	}

	LOGD("Stop decoder...");
	mGotEOS = true;
	EnqueuePacket(NULL, 0, 0);
	pthread_join(mDecoderThread, NULL);

	delete mDecoder;
	mDecoder = NULL;

	FreeBuffers();
	mNumCapBuffers = 0;
	mCurVideoOutputIndex = 0;
	mCurVideoDMAFDIndex = 0;
	mFormatWidth = 0;
	mFormatHeight = 0;
	mGotEOS = 0;
	mPreloadBuffersIndex = 0;
	mBufferColorFormat = NVBUF_COLOR_FORMAT_INVALID;

	mState = STATE_READY;

	LOGD("Stop decoder... DONE");
}

void zznvcodec_decoder_blocking::EnqueuePacket(unsigned char* pBuffer, int nSize, int64_t nTimestamp) {
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *buffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(mPreloadBuffersIndex == mDecoder->output_plane.getNumBuffers()) {
		// reused
		ret = mDecoder->output_plane.dqBuffer(v4l2_buf, &buffer, NULL, 0);
		if(ret < 0) {
			// LOGE("%s(%d): Error DQing buffer at output plane, err=%d", __FUNCTION__, __LINE__, ret);
			return;
		}
	} else {
		// preload
		buffer = mDecoder->output_plane.getNthBuffer(mPreloadBuffersIndex);
		v4l2_buf.index = mPreloadBuffersIndex;
		mPreloadBuffersIndex++;
	}

	if(pBuffer) {
		memcpy(buffer->planes[0].data, pBuffer, nSize);
	} else {
		v4l2_buf.flags |= V4L2_BUF_FLAG_LAST;
	}
	buffer->planes[0].bytesused = nSize;

	v4l2_buf.m.planes[0].bytesused = buffer->planes[0].bytesused;
	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = (int)(nTimestamp / 1000000);
	v4l2_buf.timestamp.tv_usec = (int)(nTimestamp % 1000000);

#if 0
	LOGD("%s(%d): buffer: index=%d planes[0]:bytesused=%d tv_sec=%d tv_usec=%d", __FUNCTION__, __LINE__, buffer->index, buffer->planes[0].bytesused, v4l2_buf.timestamp.tv_sec, v4l2_buf.timestamp.tv_usec);
	LOGD("%s(%d): [%02X %02X %02X %02X %02X %02X %02X %02X]", __FUNCTION__, __LINE__,
		(int)((uint8_t*)buffer->planes[0].data)[0], (int)((uint8_t*)buffer->planes[0].data)[1],
		(int)((uint8_t*)buffer->planes[0].data)[2], (int)((uint8_t*)buffer->planes[0].data)[3],
		(int)((uint8_t*)buffer->planes[0].data)[4], (int)((uint8_t*)buffer->planes[0].data)[5],
		(int)((uint8_t*)buffer->planes[0].data)[6], (int)((uint8_t*)buffer->planes[0].data)[7]);
#endif

	ret = mDecoder->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0)
	{
		LOGE("%s(%d): Error Qing buffer at output plane", __FUNCTION__, __LINE__);
	}
}

void zznvcodec_decoder_blocking::SetVideoCompressionBuffer(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp) {
	if (mV4L2PixFmt == V4L2_PIX_FMT_AV1 || mV4L2PixFmt == V4L2_PIX_FMT_MJPEG) {
		EnqueuePacket(pBuffer, nSize, nTimestamp);
	} else if(mV4L2PixFmt == V4L2_PIX_FMT_H264){// for nalu input (H264)
		// find first NALu
		int start_bytes;
		while(nSize > 4) {
			if(IS_NAL_UNIT_START(pBuffer)) {
				start_bytes = 4;
				break;
			} else if(IS_NAL_UNIT_START1(pBuffer)) {
				start_bytes = 3;
				break;
			}

			pBuffer++;
			nSize--;
		}

		// find rest of NALu
		while(true) {
			unsigned char* next_nalu = pBuffer + start_bytes;
			int next_size = nSize - start_bytes;
			int next_start_bytes;
			while(next_size > 4) {
				if(IS_NAL_UNIT_START(next_nalu)) {
					next_start_bytes = 4;
					break;
				} else if(IS_NAL_UNIT_START1(next_nalu)) {
					next_start_bytes = 3;
					break;
				}

				next_nalu++;
				next_size--;
			}

			if(next_size <= 4) {
				// the last NALu
				EnqueuePacket(pBuffer, nSize, nTimestamp);
				break;
			}

			EnqueuePacket(pBuffer, (int)(next_nalu - pBuffer), nTimestamp);
			pBuffer = next_nalu;
			nSize = next_size;
			start_bytes = next_start_bytes;
		}
	} else { // for nalu input (H265)
		mV4L2PixFmt = V4L2_PIX_FMT_H265;
		// find first NALu
		int start_bytes;
		while(nSize > 4) {
			if(IS_NAL_UNIT_START(pBuffer)) {
				start_bytes = 4;
				break;
			} else if(IS_NAL_UNIT_START1(pBuffer)) {
				start_bytes = 3;
				break;
			}

			pBuffer++;
			nSize--;
		}
		bool bIsSliceHeader = false;

		// find rest of NALu
		while(true) {
			unsigned char* next_nalu = pBuffer + start_bytes;
			int next_size = nSize - start_bytes;
			int next_start_bytes;
			while(next_size > 4) {
				if(IS_NAL_UNIT_START(next_nalu)) {
					next_start_bytes = 4;
					break;
				} else if(IS_NAL_UNIT_START1(next_nalu)) {
					next_start_bytes = 3;
					break;
				}

				next_nalu++;
				next_size--;
			}
			// LOGD("pBuffer[%d] = 0x%2X", start_bytes+2, pBuffer[start_bytes+2]);
			int h265_nal_unit_type = GET_H265_NAL_UNIT_TYPE(pBuffer, start_bytes);
			if (((h265_nal_unit_type >= HEVC_NUT_TRAIL_N && h265_nal_unit_type <= HEVC_NUT_RASL_R) ||
				(h265_nal_unit_type >= HEVC_NUT_BLA_W_LP && h265_nal_unit_type <= HEVC_NUT_CRA_NUT)) &&
				GET_H265_FIRST_SLICE_SEGMENT_IN_PIC_FLAG(pBuffer, start_bytes+2/*offset to slice header*/)) {

				// LOGD("this is first slice");
				bIsSliceHeader = true;
				next_size = 0; // force all left data to enqueue

			}


			if(next_size <= 4) {
				// the last NALu
				EnqueuePacket(pBuffer, nSize, nTimestamp);
				break;
			}

			EnqueuePacket(pBuffer, (int)(next_nalu - pBuffer), nTimestamp);
			pBuffer = next_nalu;
			nSize = next_size;
			start_bytes = next_start_bytes;
		}

	}
}

void zznvcodec_decoder_blocking::SetVideoCompressionBuffer2(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp, unsigned char *pDestBuffer, int64_t *nDestBufferSize, int64_t *nDestTimestamp) {
	SetVideoCompressionBuffer(pBuffer, nSize, nFlags, nTimestamp);

#ifdef DIRECT_OUTPUT
	if (mDecodedFrames[mCurVideoOutputIndex].DestBufferSize != 0)
	{
		zznvcodec_video_frame_t *oVideoFrame = mDecodedFrames[mCurVideoOutputIndex].DestBuffer;
		int64_t nOffset = 0;
		for (int k = 0 ; k< oVideoFrame->num_planes ; k++) {
			for (int i =0 ; i< oVideoFrame->planes[k].height ; i++) {
				memcpy(pDestBuffer + i * oVideoFrame->planes[k].stride + nOffset ,oVideoFrame->planes[k].ptr + i * oVideoFrame->planes[k].stride, oVideoFrame->planes[k].stride);
			}
			nOffset += oVideoFrame->planes[k].height * oVideoFrame->planes[k].width;
		}

		*nDestBufferSize = mDecodedFrames[mCurVideoOutputIndex].DestBufferSize;
		*nDestTimestamp = mDecodedFrames[mCurVideoOutputIndex].TimeStamp;
		mDecodedFrames[mCurVideoOutputIndex].DestBufferSize = 0;
		mCurVideoOutputIndex = (mCurVideoOutputIndex + 1) % MAX_VIDEO_BUFFERS;
	}
#endif
}

int zznvcodec_decoder_blocking::GetVideoCompressionBuffer(zznvcodec_video_frame_t** ppFrame, int64_t* pTimestamp) {
	TODO_TAG();

	return -1;
}

void* zznvcodec_decoder_blocking::_DecodeMain(void* arg) {
	zznvcodec_decoder_blocking* pThis = (zznvcodec_decoder_blocking*)arg;

	return pThis->DecoderMain();
}

void* zznvcodec_decoder_blocking::DecoderMain() {
	int ret, err;
	struct v4l2_event ev;

	LOGD("%s: begins", __FUNCTION__);

	bool bGotV4L2Error = false;
	bool bGotTransError = false;
	bool got_resolution = false;
	while(! got_resolution && !bGotV4L2Error) {
		LOGD("%s(%d): wait for V4L2_EVENT_RESOLUTION_CHANGE...", __FUNCTION__, __LINE__);
		ret = mDecoder->dqEvent(ev, 50000);
		if (ret == 0)
		{
			switch (ev.type)
			{
				case V4L2_EVENT_RESOLUTION_CHANGE:
					LOGD("%s(%d): New V4L2_EVENT_RESOLUTION_CHANGE received!!", __FUNCTION__, __LINE__);
					QueryAndSetCapture();
					got_resolution = true;
					break;

				default:
					LOGE("%s(%d): unexpected value, ev.type=%d", __FUNCTION__, __LINE__, ev.type);
					break;
			}
		} else {
			LOGE("%s(%d): failed to received V4L2_EVENT_RESOLUTION_CHANGE, ret=%d", __FUNCTION__, __LINE__, ret);
			bGotV4L2Error = true;
		}
	}

	LOGD("start decoding... error=%d, isInError=%d, EOS=%d", bGotV4L2Error, mDecoder->isInError(), mGotEOS);

	while (!(bGotV4L2Error || mDecoder->isInError() || mGotEOS)) {
		NvBuffer *dec_buffer;
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));
		v4l2_buf.m.planes = planes;

		// Dequeue a filled buffer
		if (mDecoder->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
		{
			err = errno;
			if (err == EAGAIN)
			{
				if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
				{
					LOGD("Got EoS at capture plane");
					break;
				}

				usleep(1000);
				continue;
			}
			else
			{
				if(! mGotEOS) {
					LOGE("%s(%d): Error while calling dequeue at capture plane, errno=%d", __FUNCTION__, __LINE__, err);
				}
			}

			bGotV4L2Error = true;
			break;
		}

		/* Clip & Stitch can be done by adjusting rectangle */
		NvBufSurf::NvCommonTransformParams transform_params;
		transform_params.src_top = 0;
		transform_params.src_left = 0;
		transform_params.src_width = mFormatWidth;
		transform_params.src_height = mFormatHeight;
		transform_params.dst_top = 0;
		transform_params.dst_left = 0;
		transform_params.dst_width = mFormatWidth;
		transform_params.dst_height = mFormatHeight;
		transform_params.flag = NVBUFSURF_TRANSFORM_FILTER;
		transform_params.flip = NvBufSurfTransform_None;
		transform_params.filter = NvBufSurfTransformInter_Nearest;

		dec_buffer->planes[0].fd = mDMABufFDs[v4l2_buf.index];

		// ring video frame buffer
		int dst_fd = mVideoDMAFDs[mCurVideoDMAFDIndex];
		zznvcodec_video_frame_t& oVideoFrame = mVideoFrames[mCurVideoDMAFDIndex];

		/* Perform Blocklinear to PitchLinear conversion. */
		ret = NvBufSurf::NvTransform(&transform_params, dec_buffer->planes[0].fd, dst_fd);
		if (ret < 0) {
			LOGE("%s(%d): NvBufSurf::NvTransform failed, err=%d", __FUNCTION__, __LINE__, ret);
			bGotTransError = true;
			break;
		}

#if 0 // DEBUG
		{
			static int t = 0;

			LOGW("%s(%d): t=%d", __FUNCTION__, __LINE__, t);
			if(t++ >= 5) {
				t = 0;
				bGotTransError = true;
				LOGW("%s(%d): BREAK!!!", __FUNCTION__, __LINE__);

				break;
			}
		}
#endif

		v4l2_buf.m.planes[0].m.fd = mDMABufFDs[v4l2_buf.index];
		if (mDecoder->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
		{
			LOGE("%s(%d): Error while queueing buffer at decoder capture plane", __FUNCTION__, __LINE__);
			bGotV4L2Error = true;
			break;
		}

#if 0
		LOGD("%s(%d): dst_fd=%d planes[%d]={%p %p %p} w{%d %d %d} h{%d %d %d} s{%d %d %d}", __FUNCTION__, __LINE__, dst_fd,
			oVideoFrame.num_planes, oVideoFrame.planes[0].ptr, oVideoFrame.planes[1].ptr, oVideoFrame.planes[2].ptr,
			oVideoFrame.planes[0].width, oVideoFrame.planes[1].width, oVideoFrame.planes[2].width,
			oVideoFrame.planes[0].height, oVideoFrame.planes[1].height, oVideoFrame.planes[2].height,
			oVideoFrame.planes[0].stride, oVideoFrame.planes[1].stride, oVideoFrame.planes[2].stride);
#endif

		if(! bGotTransError) {
#ifdef DIRECT_OUTPUT
			mDecodedFrames[mCurVideoDMAFDIndex].DestBuffer = &oVideoFrame;
			mDecodedFrames[mCurVideoDMAFDIndex].DestBufferSize = mDecodedFrameSize;
			mDecodedFrames[mCurVideoDMAFDIndex].TimeStamp = v4l2_buf.timestamp.tv_sec * 1000000LL + v4l2_buf.timestamp.tv_usec;
			LOGD("curIndex:%d time:%d size:%d", mCurVideoDMAFDIndex ,mDecodedFrames[mCurVideoDMAFDIndex].TimeStamp / 1000.0, mDecodedFrames[mCurVideoDMAFDIndex].DestBufferSize);
#else
			if(mOnVideoFrame) {
				int64_t pts = v4l2_buf.timestamp.tv_sec * 1000000LL + v4l2_buf.timestamp.tv_usec;
				mOnVideoFrame(&oVideoFrame, pts, mOnVideoFrame_User);
			}
#endif
			mCurVideoDMAFDIndex = (mCurVideoDMAFDIndex + 1) % MAX_VIDEO_BUFFERS;
		}
	}

	LOGD("%s: ends", __FUNCTION__);

	return NULL;
}

void zznvcodec_decoder_blocking::QueryAndSetCapture() {
	int ret;
	struct v4l2_format format;
	struct v4l2_crop crop;
	uint32_t sar_width;
	uint32_t sar_height;
	int32_t min_dec_capture_buffers;
	NvBufSurf::NvCommonAllocateParams params;
	NvBufSurf::NvCommonAllocateParams capParams;

	ret = mDecoder->capture_plane.getFormat(format);
	ret = mDecoder->capture_plane.getCrop(crop);
	LOGD("Video Resolution: %d x %d (PixFmt=%08X, %dx%d)", crop.c.width, crop.c.height,
		format.fmt.pix_mp.pixelformat, format.fmt.pix_mp.width, format.fmt.pix_mp.height);

	ret = mDecoder->getSAR(sar_width, sar_height);
	LOGD("Video SAR: %d x %d", sar_width, sar_height);

	/* deinitPlane unmaps the buffers and calls REQBUFS with count 0 */
	mDecoder->capture_plane.deinitPlane();
	FreeBuffers();

	ret = mDecoder->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat, format.fmt.pix_mp.width, format.fmt.pix_mp.height);
	if(ret < 0) {
		LOGE("%s(%d): mDecoder->setCapturePlaneFormat failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	mFormatHeight = format.fmt.pix_mp.height;
	mFormatWidth = format.fmt.pix_mp.width;

	/* Create PitchLinear output buffer for transform. */
	params.memType = NVBUF_MEM_SURFACE_ARRAY;
	params.width = crop.c.width;
	params.height = crop.c.height;
	params.layout = NVBUF_LAYOUT_PITCH;
	params.colorFormat = mBufferColorFormat;
	params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
	ret = NvBufSurf::NvAllocate(&params, _countof(mVideoDMAFDs), mVideoDMAFDs);
	if(ret < 0) {
		LOGE("%s(%d): NvBufSurf::NvAllocate failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	for(int i = 0;i < _countof(mVideoDMAFDs);i++) {
		if(mVideoDMAFDs[i] == -1)
			continue;

		NvBufSurface *nvbuf_surf = NULL;
		ret = NvBufSurfaceFromFd(mVideoDMAFDs[i], (void**)(&nvbuf_surf));
		if (ret < 0) {
			LOGE("%s(%d): NvBufSurfaceFromFd failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

		ret = NvBufSurfaceMap(nvbuf_surf, 0, -1, NVBUF_MAP_READ_WRITE);
		if (ret < 0) {
			LOGE("%s(%d): NvBufSurfaceFromFd failed, err=%d", __FUNCTION__, __LINE__, ret);
		}

#if 0
		LOGD("%d: %dx%dx%d %d %d [0]=%dx%dx%d,%p [1]=%dx%dx%d,%p", i,
			(int)nvbuf_surf->surfaceList->width,
			(int)nvbuf_surf->surfaceList->height,
			(int)nvbuf_surf->surfaceList->pitch,
			(int)nvbuf_surf->surfaceList->colorFormat,
			(int)nvbuf_surf->surfaceList->planeParams.num_planes,
			(int)nvbuf_surf->surfaceList->planeParams.width[0],
			(int)nvbuf_surf->surfaceList->planeParams.height[0],
			(int)nvbuf_surf->surfaceList->planeParams.pitch[0],
			nvbuf_surf->surfaceList->mappedAddr.addr[0],
			(int)nvbuf_surf->surfaceList->planeParams.width[1],
			(int)nvbuf_surf->surfaceList->planeParams.height[1],
			(int)nvbuf_surf->surfaceList->planeParams.pitch[1],
			nvbuf_surf->surfaceList->mappedAddr.addr[1]);
#endif

		zznvcodec_video_frame_t& oVideoFrame = mVideoFrames[i];
		oVideoFrame.num_planes = nvbuf_surf->surfaceList->planeParams.num_planes;
		for(int i = 0;i < oVideoFrame.num_planes;++i) {
			oVideoFrame.planes[i].width = nvbuf_surf->surfaceList->planeParams.width[i];
			oVideoFrame.planes[i].height = nvbuf_surf->surfaceList->planeParams.height[i];
			oVideoFrame.planes[i].ptr = (uint8_t*)nvbuf_surf->surfaceList->mappedAddr.addr[i];
			oVideoFrame.planes[i].stride = nvbuf_surf->surfaceList->planeParams.pitch[i];
		}
	}

	ret = mDecoder->getMinimumCapturePlaneBuffers(min_dec_capture_buffers);
	if(ret < 0) {
		LOGE("%s(%d): mDecoder->getMinimumCapturePlaneBuffers failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	mNumCapBuffers = min_dec_capture_buffers + 1;
	// LOGD("mNumCapBuffers=%d", mNumCapBuffers);

	/* Set colorformats for relevant colorspaces. */
	NvBufSurfaceColorFormat pix_format;
	switch(format.fmt.pix_mp.colorspace)
	{
	case V4L2_COLORSPACE_SMPTE170M:
		if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
		{
			LOGD("Decoder colorspace ITU-R BT.601 with standard range luma (16-235)");
			pix_format = NVBUF_COLOR_FORMAT_NV12;
		}
		else
		{
			LOGD("Decoder colorspace ITU-R BT.601 with extended range luma (0-255)");
			pix_format = NVBUF_COLOR_FORMAT_NV12_ER;
		}
		break;
	case V4L2_COLORSPACE_REC709:
		if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
		{
			LOGD("Decoder colorspace ITU-R BT.709 with standard range luma (16-235)");
			pix_format =  NVBUF_COLOR_FORMAT_NV12_709;
		}
		else
		{
			LOGD("Decoder colorspace ITU-R BT.709 with extended range luma (0-255)");
			pix_format = NVBUF_COLOR_FORMAT_NV12_709_ER;
		}
		break;
	case V4L2_COLORSPACE_BT2020:
		{
			LOGD("Decoder colorspace ITU-R BT.2020");
			pix_format = NVBUF_COLOR_FORMAT_NV12_2020;
		}
		break;
	default:
		LOGD("supported colorspace details not available, use default");
		if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
		{
			LOGD("Decoder colorspace ITU-R BT.601 with standard range luma (16-235)");
			pix_format = NVBUF_COLOR_FORMAT_NV12;
		}
		else
		{
			LOGD("Decoder colorspace ITU-R BT.601 with extended range luma (0-255)");
			pix_format = NVBUF_COLOR_FORMAT_NV12_ER;
		}
		break;
	}

	params.memType = NVBUF_MEM_SURFACE_ARRAY;
	params.width = crop.c.width;
	params.height = crop.c.height;
	params.layout = NVBUF_LAYOUT_BLOCK_LINEAR;
	params.memtag = NvBufSurfaceTag_VIDEO_DEC;

    if (format.fmt.pix_mp.pixelformat  == V4L2_PIX_FMT_NV24M)
      pix_format = NVBUF_COLOR_FORMAT_NV24;
    else if (format.fmt.pix_mp.pixelformat  == V4L2_PIX_FMT_NV24_10LE)
      pix_format = NVBUF_COLOR_FORMAT_NV24_10LE;

    params.colorFormat = pix_format;

	ret = NvBufSurf::NvAllocate(&params, mNumCapBuffers, mDMABufFDs);
	if(ret < 0) {
		LOGE("%s(%d): NvBufSurf::NvAllocate failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = mDecoder->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, mNumCapBuffers);
	if(ret < 0) {
		LOGE("%s(%d): mDecoder->capture_plane.reqbufs failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	ret = mDecoder->capture_plane.setStreamStatus(true);
	if(ret < 0) {
		LOGE("%s(%d): mDecoder->capture_plane.setStreamStatus failed, err=%d", __FUNCTION__, __LINE__, ret);
	}

	for(uint32_t i = 0; i < mDecoder->capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		v4l2_buf.m.planes[0].m.fd = mDMABufFDs[i];
		ret = mDecoder->capture_plane.qBuffer(v4l2_buf, NULL);
		if(ret < 0) {
			LOGE("%s(%d): mDecoder->capture_plane.qBuffer failed, err=%d", __FUNCTION__, __LINE__, ret);
		}
	}
}
