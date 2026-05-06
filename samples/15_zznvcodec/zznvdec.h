#ifndef __ZZ_NVDEC_H__
#define __ZZ_NVDEC_H__

#include "zznvcodec.h"

struct zznvcodec_decoder_t {
	typedef zznvcodec_decoder_t self_t;

	explicit zznvcodec_decoder_t();
	virtual ~zznvcodec_decoder_t();

	virtual void SetVideoProperty(int nWidth, int nHeight, zznvcodec_pixel_format_t nFormat);
	virtual void SetMiscProperty(int nProperty, intptr_t pValue);
	virtual void RegisterCallbacks(zznvcodec_decoder_on_video_frame_t pCB, intptr_t pUser);

	virtual int Start() = 0;
	virtual void Stop() = 0;
	virtual void SetVideoCompressionBuffer(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp) = 0;
	virtual void SetVideoCompressionBuffer2(unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp, unsigned char *pDestBuffer, int64_t *nDestBufferSize, int64_t *nDestTimestamp) = 0;
	virtual int GetVideoCompressionBuffer(zznvcodec_video_frame_t** ppFrame, int64_t* pTimestamp) = 0;

protected:
	int mWidth;
	int mHeight;
	zznvcodec_pixel_format_t mFormat;
	zznvcodec_decoder_on_video_frame_t mOnVideoFrame;
	intptr_t mOnVideoFrame_User;

	zznvcodec_codec_type_t mCodecType;
	bool mLOWLATENCY;

	void OnVideoFrame(zznvcodec_video_frame_t* pFrame, int64_t nTimestamp);
};

extern zznvcodec_decoder_t* NewDecoder_blocking();
extern zznvcodec_decoder_t* NewDecoder_nonblocking();

#endif // __ZZ_NVDEC_H__