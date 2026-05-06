#include "zznvdec.h"
#include "ZzLog.h"

ZZ_INIT_LOG("zznvdec");

zznvcodec_decoder_t::zznvcodec_decoder_t() {
	mWidth = 0;
	mHeight = 0;
	mFormat = ZZNVCODEC_PIXEL_FORMAT_UNKNOWN;
	mOnVideoFrame = NULL;
	mOnVideoFrame_User = 0;

	mCodecType = ZZNVCODEC_CODEC_TYPE_UNKNOWN;
	mLOWLATENCY = 1;
}

zznvcodec_decoder_t::~zznvcodec_decoder_t() {
}

void zznvcodec_decoder_t::SetVideoProperty(int nWidth, int nHeight, zznvcodec_pixel_format_t nFormat) {
	mWidth = nWidth;
	mHeight = nHeight;
	mFormat = nFormat;
}

void zznvcodec_decoder_t::SetMiscProperty(int nProperty, intptr_t pValue) {
	switch(nProperty) {
	case ZZNVCODEC_PROP_CODEC_TYPE:
		mCodecType = *(zznvcodec_codec_type_t*)pValue;
		break;

	case ZZNVCODEC_PROP_LOWLATENCY:
		mLOWLATENCY = *(int*)pValue;
		break;

	default:
		LOGE("%s(%d): unexpected value, nProperty = %d", __FUNCTION__, __LINE__, nProperty);
		break;
	}
}

void zznvcodec_decoder_t::RegisterCallbacks(zznvcodec_decoder_on_video_frame_t pCB, intptr_t pUser) {
	mOnVideoFrame = pCB;
	mOnVideoFrame_User = pUser;
}

void zznvcodec_decoder_t::OnVideoFrame(zznvcodec_video_frame_t* pFrame, int64_t nTimestamp) {
	mOnVideoFrame(pFrame, nTimestamp, mOnVideoFrame_User);
}

zznvcodec_decoder_t* zznvcodec_decoder_new() {
	return zznvcodec_decoder_new1(ZZNVCODEC_BACKEND_TYPE_BLOCKING);
}

zznvcodec_decoder_t* zznvcodec_decoder_new1(int nBackendType) {
	zznvcodec_decoder_t* pDecoder;

	switch(nBackendType) {
	case ZZNVCODEC_BACKEND_TYPE_BLOCKING:
		pDecoder = NewDecoder_blocking();
		break;

	case ZZNVCODEC_BACKEND_TYPE_NONBLOCKING:
		pDecoder = NewDecoder_nonblocking();
		break;

	default:
		LOGE("%s(%d): unexpected value, nBackendType=%d", __FUNCTION__, __LINE__, nBackendType);
		pDecoder = NULL;
		break;
	}

	return pDecoder;
}

void zznvcodec_decoder_delete(zznvcodec_decoder_t* pThis) {
	delete pThis;
}

void zznvcodec_decoder_set_video_property(zznvcodec_decoder_t* pThis, int nWidth, int nHeight, zznvcodec_pixel_format_t nFormat) {
	pThis->SetVideoProperty(nWidth, nHeight, nFormat);
}

void zznvcodec_decoder_set_misc_property(zznvcodec_decoder_t* pThis, int nProperty, intptr_t pValue) {
	pThis->SetMiscProperty(nProperty, pValue);
}

void zznvcodec_decoder_register_callbacks(zznvcodec_decoder_t* pThis, zznvcodec_decoder_on_video_frame_t pCB, intptr_t pUser) {
	pThis->RegisterCallbacks(pCB, pUser);
}

int zznvcodec_decoder_start(zznvcodec_decoder_t* pThis) {
	return pThis->Start();
}

void zznvcodec_decoder_stop(zznvcodec_decoder_t* pThis) {
	pThis->Stop();
}

void zznvcodec_decoder_set_video_compression_buffer(zznvcodec_decoder_t* pThis, unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp) {
	pThis->SetVideoCompressionBuffer(pBuffer, nSize, nFlags, nTimestamp);
}

void zznvcodec_decoder_set_video_compression_buffer2(zznvcodec_decoder_t* pThis, unsigned char* pBuffer, int nSize, int nFlags, int64_t nTimestamp, unsigned char *pDestBuffer, int64_t *nDestBufferSize, int64_t *nDestTimestamp) {
	pThis->SetVideoCompressionBuffer2(pBuffer, nSize, nFlags, nTimestamp, pDestBuffer, nDestBufferSize, nDestTimestamp);
}

int zznvcodec_decoder_get_video_uncompression_buffer(zznvcodec_decoder_t* pThis, zznvcodec_video_frame_t** ppFrame, int64_t* pTimestamp) {
	return pThis->GetVideoCompressionBuffer(ppFrame, pTimestamp);
}
