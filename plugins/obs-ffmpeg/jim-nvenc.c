#define GL_GLEXT_PROTOTYPES 1

#include "jim-nvenc.h"
#include <util/circlebuf.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-avc.h>
#include <libavutil/rational.h>
#ifdef _WIN32
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#else
#include <graphics/graphics.h>
#include <GL/gl.h>
#endif

/* ========================================================================= */

#define EXTRA_BUFFERS 5

#define do_log(level, format, ...)               \
	blog(level, "[jim-nvenc: '%s'] " format, \
	     obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define error_hr(msg) error("%s: %s: 0x%08lX", __FUNCTION__, msg, (uint32_t)hr);

struct nv_bitstream;
struct nv_texture;

struct handle_tex {
	uint32_t handle;
#ifdef _WIN32
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km;
#else
	GLuint tex[2];
#endif
};

/* ------------------------------------------------------------------------- */
/* Main Implementation Structure                                             */

struct nvenc_data {
	obs_encoder_t *encoder;

	void *session;
	NV_ENC_INITIALIZE_PARAMS params;
	NV_ENC_CONFIG config;
	int rc_lookahead;
	int buf_count;
	int output_delay;
	int buffers_queued;
	size_t next_bitstream;
	size_t cur_bitstream;
	bool encode_started;
	bool first_packet;
	bool can_change_bitrate;
	int32_t bframes;

	DARRAY(struct nv_bitstream) bitstreams;
	DARRAY(struct nv_texture) textures;
#ifdef _WIN32
	DARRAY(struct handle_tex) input_textures;
#endif
	struct circlebuf dts_list;

	DARRAY(uint8_t) packet_data;
	int64_t packet_pts;
	bool packet_keyframe;

#ifdef _WIN32
	ID3D11Device *device;
	ID3D11DeviceContext *context;
#endif

	uint32_t cx;
	uint32_t cy;

	uint8_t *header;
	size_t header_size;

	uint8_t *sei;
	size_t sei_size;
};

/* ------------------------------------------------------------------------- */
/* Bitstream Buffer                                                          */

struct nv_bitstream {
	void *ptr;
#ifdef _WIN32
	HANDLE event;
#endif
};

#define NV_FAILED(x) nv_failed(enc->encoder, x, __FUNCTION__, #x)

static bool nv_bitstream_init(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	NV_ENC_CREATE_BITSTREAM_BUFFER buf = {
		NV_ENC_CREATE_BITSTREAM_BUFFER_VER};

	if (NV_FAILED(nv.nvEncCreateBitstreamBuffer(enc->session, &buf))) {
		return false;
	}

	bs->ptr = buf.bitstreamBuffer;

#ifdef _WIN32
	NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
	HANDLE event = NULL;

	event = CreateEvent(NULL, true, true, NULL);
	if (!event) {
		error("%s: %s", __FUNCTION__, "Failed to create event");
		goto fail;
	}

	params.completionEvent = event;
	if (NV_FAILED(nv.nvEncRegisterAsyncEvent(enc->session, &params))) {
		goto fail;
	}

	bs->event = event;
#endif

	return true;

#ifdef _WIN32
fail:
	if (event) {
		CloseHandle(event);
	}
	if (buf.bitstreamBuffer) {
		nv.nvEncDestroyBitstreamBuffer(enc->session,
					       buf.bitstreamBuffer);
	}
	return false;
#endif
}

static void nv_bitstream_free(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	if (bs->ptr) {
		nv.nvEncDestroyBitstreamBuffer(enc->session, bs->ptr);

#ifdef _WIN32
		NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
		params.completionEvent = bs->event;
		nv.nvEncUnregisterAsyncEvent(enc->session, &params);
		CloseHandle(bs->event);
#endif
	}
}

/* ------------------------------------------------------------------------- */
/* Texture Resource                                                          */

struct nv_texture {
	void *res;
#ifdef _WIN32
	ID3D11Texture2D *tex;
#else
	GLuint tex;
#endif
	bool is_nv12;
	void *mapped_res;
};

#ifdef _WIN32

static bool complete_texture(struct nvenc_data *enc, struct nv_texture *nvtex,
			     bool nv12)
{
	ID3D11Device *device = enc->device;
	ID3D11Texture2D *tex;
	HRESULT hr;

	if (nvtex->is_nv12 == nv12 && nvtex->tex) {
		return true;
	}

	if (nvtex->mapped_res) {
		nv.nvEncUnmapInputResource(enc->session, nvtex->mapped_res);
		nvtex->mapped_res = NULL;
	}
	if (nvtex->res) {
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
		nvtex->res = NULL;
	}
	if (nvtex->tex) {
		nvtex->tex->lpVtbl->Release(nvtex->tex);
		nvtex->tex = NULL;
	}

	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = enc->cx;
	desc.Height = enc->cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;

	hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, &tex);
	if (FAILED(hr)) {
		error_hr("Failed to create texture");
		return false;
	}

	tex->lpVtbl->SetEvictionPriority(tex, DXGI_RESOURCE_PRIORITY_MAXIMUM);

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	res.resourceToRegister = tex;
	res.width = enc->cx;
	res.height = enc->cy;
	res.bufferFormat = nv12 ? NV_ENC_BUFFER_FORMAT_NV12
				: NV_ENC_BUFFER_FORMAT_ABGR;

	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		tex->lpVtbl->Release(tex);
		return false;
	}

	nvtex->res = res.registeredResource;
	nvtex->tex = tex;
	nvtex->is_nv12 = nv12;
	return true;
}

static bool nv_texture_init(struct nvenc_data *enc, struct nv_texture *nvtex,
			    bool nv12)
{
	nvtex->res = NULL;
	nvtex->tex = NULL;
	nvtex->is_nv12 = false;
	nvtex->mapped_res = NULL;
	return complete_texture(enc, nvtex, nv12);
}

#else /* defined(_WIN32) */

static bool impl_gl_error(const char *func, int line)
{
	GLenum gl_err = glGetError();
	if (gl_err == GL_NO_ERROR) {
		return false;
	}

	do {
		blog(LOG_ERROR, "%s:%d: OpenGL error: 0x%x", func, line,
		     gl_err);
	} while ((gl_err = glGetError()) != GL_NO_ERROR);

	return true;
}
#define gl_error() impl_gl_error(__FUNCTION__, __LINE__)

#define check_error()                 \
	do {                          \
		if (gl_error()) {     \
			return false; \
		}                     \
	} while (0)

static bool setup_texture(GLuint tex, uint32_t width, uint32_t height,
			  bool nv12)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	check_error();
	if (nv12) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width,
			     height + height / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
			     NULL);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
			     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	check_error();
	/*glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 1);
	check_error();*/
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	return true;
}

#undef check_error

static bool complete_texture(struct nvenc_data *enc, struct nv_texture *nvtex,
			     bool nv12)
{
	GLuint tex;

	if (nvtex->is_nv12 == nv12 && nvtex->res) {
		return true;
	}

	if (gl_error()) {
		goto gl_enter_err;
	}

	if (nvtex->mapped_res) {
		nv.nvEncUnmapInputResource(enc->session, nvtex->mapped_res);
		nvtex->mapped_res = NULL;
	}
	if (nvtex->res) {
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
		nvtex->res = NULL;
		glDeleteTextures(1, &nvtex->tex);
	}

	if (gl_error()) {
		goto gl_enter_err;
	}

	glGenTextures(1, &tex);
	if (gl_error()) {
		goto gen_tex_err;
	}

	if (!setup_texture(tex, enc->cx, enc->cy, nv12)) {
		goto setup_tex_err;
	}

	NV_ENC_INPUT_RESOURCE_OPENGL_TEX texRes = {tex, GL_TEXTURE_2D};

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX;
	res.resourceToRegister = (void *)&texRes;
	res.width = enc->cx;
	res.height = enc->cy;
	res.pitch = nv12 ? enc->cx : enc->cx * 4;
	res.bufferFormat = nv12 ? NV_ENC_BUFFER_FORMAT_NV12
				: NV_ENC_BUFFER_FORMAT_ABGR;

	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		goto reg_tex_err;
	}

	nvtex->res = res.registeredResource;
	nvtex->tex = tex;
	nvtex->is_nv12 = nv12;
	return true;

reg_tex_err:
setup_tex_err:
	glDeleteTextures(1, &tex);
gen_tex_err:
gl_enter_err:
	return false;
}

static bool nv_texture_init(struct nvenc_data *enc, struct nv_texture *nvtex,
			    bool nv12)
{
	nvtex->res = NULL;
	nvtex->tex = 0;
	nvtex->is_nv12 = false;
	nvtex->mapped_res = NULL;
	return complete_texture(enc, nvtex, nv12);
}

#endif /* defined(_WIN32) */

static void nv_texture_free(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	if (nvtex->res) {
		if (nvtex->mapped_res) {
			nv.nvEncUnmapInputResource(enc->session,
						   nvtex->mapped_res);
		}
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
#ifdef _WIN32
		nvtex->tex->lpVtbl->Release(nvtex->tex);
#else
		glDeleteTextures(1, &nvtex->tex);
#endif
	}
}

/* ------------------------------------------------------------------------- */
/* Implementation                                                            */

static const char *nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC H.264 (new)";
}

static inline int nv_get_cap(struct nvenc_data *enc, NV_ENC_CAPS cap)
{
	if (!enc->session)
		return 0;

	NV_ENC_CAPS_PARAM param = {NV_ENC_CAPS_PARAM_VER};
	int v;

	param.capsToQuery = cap;
	nv.nvEncGetEncodeCaps(enc->session, NV_ENC_CODEC_H264_GUID, &param, &v);
	return v;
}

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_data *enc = data;

	/* Only support reconfiguration of CBR bitrate */
	if (enc->can_change_bitrate) {
		int bitrate = (int)obs_data_get_int(settings, "bitrate");

		enc->config.rcParams.averageBitRate = bitrate * 1000;
		enc->config.rcParams.maxBitRate = bitrate * 1000;

		NV_ENC_RECONFIGURE_PARAMS params = {0};
		params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
		params.reInitEncodeParams = enc->params;
		params.resetEncoder = 1;
		params.forceIDR = 1;

#ifndef _WIN32
		obs_enter_graphics();
#endif
		NVENCSTATUS s =
			nv.nvEncReconfigureEncoder(enc->session, &params);
#ifndef _WIN32
		obs_leave_graphics();
#endif
		if (NV_FAILED(s)) {
			return false;
		}
	}

	return true;
}

#ifdef _WIN32

static HANDLE get_lib(struct nvenc_data *enc, const char *lib)
{
	HMODULE mod = GetModuleHandleA(lib);
	if (mod)
		return mod;

	mod = LoadLibraryA(lib);
	if (!mod)
		error("Failed to load %s", lib);
	return mod;
}

typedef HRESULT(WINAPI *CREATEDXGIFACTORY1PROC)(REFIID, void **);

static bool init_d3d11(struct nvenc_data *enc, obs_data_t *settings)
{
	HMODULE dxgi = get_lib(enc, "DXGI.dll");
	HMODULE d3d11 = get_lib(enc, "D3D11.dll");
	CREATEDXGIFACTORY1PROC create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	IDXGIFactory1 *factory;
	IDXGIAdapter *adapter;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	HRESULT hr;

	if (!dxgi || !d3d11) {
		return false;
	}

	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(
		dxgi, "CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(
		d3d11, "D3D11CreateDevice");

	if (!create_dxgi || !create_device) {
		error("Failed to load D3D11/DXGI procedures");
		return false;
	}

	hr = create_dxgi(&IID_IDXGIFactory1, &factory);
	if (FAILED(hr)) {
		error_hr("CreateDXGIFactory1 failed");
		return false;
	}

	hr = factory->lpVtbl->EnumAdapters(factory, 0, &adapter);
	factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		error_hr("EnumAdapters failed");
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
			   D3D11_SDK_VERSION, &device, NULL, &context);
	adapter->lpVtbl->Release(adapter);
	if (FAILED(hr)) {
		error_hr("D3D11CreateDevice failed");
		return false;
	}

	enc->device = device;
	enc->context = context;
	return true;
}

#endif /* defined(_WIN32) */

static bool init_session(struct nvenc_data *enc)
{
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {
		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
#ifdef _WIN32
	params.device = enc->device;
	params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
#else
	params.deviceType = NV_ENC_DEVICE_TYPE_OPENGL;
#endif
	params.apiVersion = NVENCAPI_VERSION;

	if (NV_FAILED(nv.nvEncOpenEncodeSessionEx(&params, &enc->session))) {
		return false;
	}
	return true;
}

#ifndef _WIN32
#define max(x, y) ((x) < (y) ? (y) : (x))
#define min(x, y) ((x) > (y) ? (y) : (x))
#endif

static bool init_encoder(struct nvenc_data *enc, obs_data_t *settings,
			 bool psycho_aq)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	bool lookahead = obs_data_get_bool(settings, "lookahead");
	int bf = (int)obs_data_get_int(settings, "bf");
	bool vbr = astrcmpi(rc, "VBR") == 0;
	NVENCSTATUS err;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	enc->cx = voi->width;
	enc->cy = voi->height;

	/* -------------------------- */
	/* get preset                 */

	GUID nv_preset = NV_ENC_PRESET_DEFAULT_GUID;
	bool twopass = false;
	bool hp = false;
	bool ll = false;

	if (astrcmpi(preset, "hq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;

	} else if (astrcmpi(preset, "mq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;
		twopass = true;

	} else if (astrcmpi(preset, "hp") == 0) {
		nv_preset = NV_ENC_PRESET_HP_GUID;
		hp = true;

	} else if (astrcmpi(preset, "ll") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhq") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhp") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
		hp = true;
		ll = true;
	}

	const bool rc_lossless = astrcmpi(rc, "lossless") == 0;
	bool lossless = rc_lossless;
	if (rc_lossless) {
		lossless = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);
		if (lossless) {
			nv_preset = hp ? NV_ENC_PRESET_LOSSLESS_HP_GUID
				       : NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
		} else {
			warn("lossless encode is not supported, ignoring");
		}
	}

	/* -------------------------- */
	/* get preset default config  */

	NV_ENC_PRESET_CONFIG preset_config = {NV_ENC_PRESET_CONFIG_VER,
					      {NV_ENC_CONFIG_VER}};

	err = nv.nvEncGetEncodePresetConfig(enc->session,
					    NV_ENC_CODEC_H264_GUID, nv_preset,
					    &preset_config);
	if (nv_failed(enc->encoder, err, __FUNCTION__,
		      "nvEncGetEncodePresetConfig")) {
		return false;
	}

	/* -------------------------- */
	/* main configuration         */

	enc->config = preset_config.presetCfg;

	uint32_t gop_size =
		(keyint_sec) ? keyint_sec * voi->fps_num / voi->fps_den : 250;

	NV_ENC_INITIALIZE_PARAMS *params = &enc->params;
	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_H264 *h264_config = &config->encodeCodecConfig.h264Config;
	NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui_params =
		&h264_config->h264VUIParameters;

	int darWidth, darHeight;
	av_reduce(&darWidth, &darHeight, voi->width, voi->height, 1024 * 1024);

	memset(params, 0, sizeof(*params));
	params->version = NV_ENC_INITIALIZE_PARAMS_VER;
	params->encodeGUID = NV_ENC_CODEC_H264_GUID;
	params->presetGUID = nv_preset;
	params->encodeWidth = voi->width;
	params->encodeHeight = voi->height;
	params->darWidth = darWidth;
	params->darHeight = darHeight;
	params->frameRateNum = voi->fps_num;
	params->frameRateDen = voi->fps_den;
#ifdef _WIN32
	params->enableEncodeAsync = 1;
#endif
	params->enablePTD = 1;
	params->encodeConfig = &enc->config;
	config->gopLength = gop_size;
	config->frameIntervalP = 1 + bf;
	h264_config->idrPeriod = gop_size;

	bool repeat_headers = obs_data_get_bool(settings, "repeat_headers");
	if (repeat_headers) {
		h264_config->repeatSPSPPS = 1;
		h264_config->disableSPSPPS = 0;
		h264_config->outputAUD = 1;
	}

	h264_config->sliceMode = 3;
	h264_config->sliceModeData = 1;

	h264_config->useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;

	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;

	switch (voi->colorspace) {
	case VIDEO_CS_601:
		vui_params->colourPrimaries = 6;
		vui_params->transferCharacteristics = 6;
		vui_params->colourMatrix = 6;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		vui_params->colourPrimaries = 1;
		vui_params->transferCharacteristics = 1;
		vui_params->colourMatrix = 1;
		break;
	case VIDEO_CS_SRGB:
		vui_params->colourPrimaries = 1;
		vui_params->transferCharacteristics = 13;
		vui_params->colourMatrix = 1;
		break;
	}

	enc->bframes = bf;

	/* lookahead */
	const bool use_profile_lookahead = config->rcParams.enableLookahead;
	lookahead = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) &&
		    (lookahead || use_profile_lookahead);
	if (lookahead) {
		enc->rc_lookahead = use_profile_lookahead
					    ? config->rcParams.lookaheadDepth
					    : 8;
	}

	int buf_count = max(4, config->frameIntervalP * 2 * 2);
	if (lookahead) {
		buf_count = max(buf_count, config->frameIntervalP +
						   enc->rc_lookahead +
						   EXTRA_BUFFERS);
	}

	buf_count = min(64, buf_count);
	enc->buf_count = buf_count;

	const int output_delay = buf_count - 1;
	enc->output_delay = output_delay;

	if (lookahead) {
		const int lkd_bound = output_delay - config->frameIntervalP - 4;
		if (lkd_bound >= 0) {
			config->rcParams.enableLookahead = 1;
			config->rcParams.lookaheadDepth =
				max(enc->rc_lookahead, lkd_bound);
			config->rcParams.disableIadapt = 0;
			config->rcParams.disableBadapt = 0;
		} else {
			lookahead = false;
		}
	}

	/* psycho aq */
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ)) {
		config->rcParams.enableAQ = psycho_aq;
		config->rcParams.aqStrength = 8;
		config->rcParams.enableTemporalAQ = psycho_aq;
	} else if (psycho_aq) {
		warn("Ignoring Psycho Visual Tuning request since GPU is not capable");
	}

	/* -------------------------- */
	/* rate control               */

	enc->can_change_bitrate =
		nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE) &&
		!lookahead;

	config->rcParams.rateControlMode = twopass ? NV_ENC_PARAMS_RC_VBR_HQ
						   : NV_ENC_PARAMS_RC_VBR;

	if (astrcmpi(rc, "cqp") == 0 || rc_lossless) {
		if (lossless) {
			h264_config->qpPrimeYZeroTransformBypassFlag = 1;
			cqp = 0;
		}

		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = cqp;
		config->rcParams.constQP.qpInterB = cqp;
		config->rcParams.constQP.qpIntra = cqp;
		enc->can_change_bitrate = false;

		bitrate = 0;
		max_bitrate = 0;

	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		h264_config->outputBufferingPeriodSEI = 1;
		config->rcParams.rateControlMode =
			twopass ? NV_ENC_PARAMS_RC_2_PASS_QUALITY
				: NV_ENC_PARAMS_RC_CBR;
	}

	h264_config->outputPictureTimingSEI = 1;
	config->rcParams.averageBitRate = bitrate * 1000;
	config->rcParams.maxBitRate = vbr ? max_bitrate * 1000 : bitrate * 1000;
	config->rcParams.vbvBufferSize = bitrate * 1000;

	/* -------------------------- */
	/* profile                    */

	if (astrcmpi(profile, "main") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
	} else if (astrcmpi(profile, "baseline") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
	} else if (!lossless) {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
	}

	/* -------------------------- */
	/* initialize                 */

	if (NV_FAILED(nv.nvEncInitializeEncoder(enc->session, params))) {
		return false;
	}

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\t2-pass:       %s\n"
	     "\tb-frames:     %d\n"
	     "\tlookahead:    %s\n"
	     "\tpsycho_aq:    %s\n",
	     rc, bitrate, cqp, gop_size, preset, profile, enc->cx, enc->cy,
	     twopass ? "true" : "false", bf, lookahead ? "true" : "false",
	     psycho_aq ? "true" : "false");

	return true;
}

static bool init_bitstreams(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (int i = 0; i < enc->buf_count; i++) {
		struct nv_bitstream bitstream;
		if (!nv_bitstream_init(enc, &bitstream)) {
			return false;
		}

		da_push_back(enc->bitstreams, &bitstream);
	}

	return true;
}

static bool init_textures(struct nvenc_data *enc)
{
	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	da_reserve(enc->bitstreams, enc->buf_count);
	for (int i = 0; i < enc->buf_count; i++) {
		struct nv_texture texture;
		if (!nv_texture_init(enc, &texture,
				     voi->format == VIDEO_FORMAT_NV12)) {
			return false;
		}

		da_push_back(enc->textures, &texture);
	}

	return true;
}

static void nvenc_destroy(void *data);

static void *nvenc_create_internal(obs_data_t *settings, obs_encoder_t *encoder,
				   bool psycho_aq)
{
	NV_ENCODE_API_FUNCTION_LIST init = {NV_ENCODE_API_FUNCTION_LIST_VER};
	struct nvenc_data *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;

	if (!init_nvenc(encoder)) {
		goto fail;
	}
	if (NV_FAILED(nv_create_instance(&init))) {
		goto fail;
	}
#ifdef _WIN32
	if (!init_d3d11(enc, settings)) {
		goto fail;
	}
#endif
	if (!init_session(enc)) {
		goto fail;
	}
	if (!init_encoder(enc, settings, psycho_aq)) {
		goto fail;
	}
	if (!init_bitstreams(enc)) {
		goto fail;
	}
	if (!init_textures(enc)) {
		goto fail;
	}

	return enc;

fail:
	nvenc_destroy(enc);
	return NULL;
}

static bool is_format_supported(enum video_format format)
{
#ifdef _WIN32
	return format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_RGBA ||
	       format == VIDEO_FORMAT_BGRA || format == VIDEO_FORMAT_BGRX;
#else
	return format == VIDEO_FORMAT_RGBA || format == VIDEO_FORMAT_BGRA ||
	       format == VIDEO_FORMAT_BGRX;
#endif
}

static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	/* this encoder requires shared textures, this cannot be used on a
	 * gpu other than the one OBS is currently running on. */
	const int gpu = (int)obs_data_get_int(settings, "gpu");
	if (gpu != 0) {
		blog(LOG_INFO,
		     "[jim-nvenc] different GPU selected by user, falling back to ffmpeg");
		goto reroute;
	}

	if (obs_encoder_scaling_enabled(encoder)) {
		blog(LOG_INFO,
		     "[jim-nvenc] scaling enabled, falling back to ffmpeg");
		goto reroute;
	}

	video_t *video = obs_encoder_video(encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	if (!is_format_supported(voi->format)) {
		blog(LOG_INFO,
		     "[jim-nvenc] unsupported video format, falling back to ffmpeg");
		goto reroute;
	}

	const bool psycho_aq = obs_data_get_bool(settings, "psycho_aq");
#ifndef _WIN32
	obs_enter_graphics();
#endif
	struct nvenc_data *enc =
		nvenc_create_internal(settings, encoder, psycho_aq);
	if ((enc == NULL) && psycho_aq) {
		blog(LOG_WARNING, "[jim-nvenc] nvenc_create_internal failed, "
				  "trying again without Psycho Visual Tuning");
		enc = nvenc_create_internal(settings, encoder, false);
	}
#ifndef _WIN32
	obs_leave_graphics();
#endif

	if (enc) {
		return enc;
	}

reroute:
	return obs_encoder_create_rerouted(encoder, "ffmpeg_nvenc");
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize);

static void nvenc_destroy(void *data)
{
	struct nvenc_data *enc = data;

#ifndef _WIN32
	obs_enter_graphics();
#endif

	if (enc->encode_started) {
		NV_ENC_PIC_PARAMS params = {NV_ENC_PIC_PARAMS_VER};
		params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
#ifdef _WIN32
		size_t next_bitstream = enc->next_bitstream;
		HANDLE next_event = enc->bitstreams.array[next_bitstream].event;
		params.completionEvent = next_event;
#endif
		nv.nvEncEncodePicture(enc->session, &params);
		get_encoded_packet(enc, true);
	}
	for (size_t i = 0; i < enc->textures.num; i++) {
		nv_texture_free(enc, &enc->textures.array[i]);
	}
	for (size_t i = 0; i < enc->bitstreams.num; i++) {
		nv_bitstream_free(enc, &enc->bitstreams.array[i]);
	}
	if (enc->session) {
		nv.nvEncDestroyEncoder(enc->session);
	}
#ifdef _WIN32
	for (size_t i = 0; i < enc->input_textures.num; i++) {
		ID3D11Texture2D *tex = enc->input_textures.array[i].tex;
		IDXGIKeyedMutex *km = enc->input_textures.array[i].km;
		tex->lpVtbl->Release(tex);
		km->lpVtbl->Release(km);
	}
	if (enc->context) {
		enc->context->lpVtbl->Release(enc->context);
	}
	if (enc->device) {
		enc->device->lpVtbl->Release(enc->device);
	}
#endif

#ifndef _WIN32
	obs_leave_graphics();
#endif

	bfree(enc->header);
	bfree(enc->sei);
	circlebuf_free(&enc->dts_list);
	da_free(enc->textures);
	da_free(enc->bitstreams);
#ifdef _WIN32
	da_free(enc->input_textures);
#endif
	da_free(enc->packet_data);
	bfree(enc);
}

#ifdef _WIN32

static ID3D11Texture2D *get_tex_from_handle(struct nvenc_data *enc,
					    uint32_t handle,
					    IDXGIKeyedMutex **km_out)
{
	ID3D11Device *device = enc->device;
	IDXGIKeyedMutex *km;
	ID3D11Texture2D *input_tex;
	HRESULT hr;

	for (size_t i = 0; i < enc->input_textures.num; i++) {
		struct handle_tex *ht = &enc->input_textures.array[i];
		if (ht->handle == handle) {
			*km_out = ht->km;
			return ht->tex;
		}
	}

	hr = device->lpVtbl->OpenSharedResource(device,
						(HANDLE)(uintptr_t)handle,
						&IID_ID3D11Texture2D,
						&input_tex);
	if (FAILED(hr)) {
		error_hr("OpenSharedResource failed");
		return NULL;
	}

	hr = input_tex->lpVtbl->QueryInterface(input_tex, &IID_IDXGIKeyedMutex,
					       &km);
	if (FAILED(hr)) {
		error_hr("QueryInterface(IDXGIKeyedMutex) failed");
		input_tex->lpVtbl->Release(input_tex);
		return NULL;
	}

	input_tex->lpVtbl->SetEvictionPriority(input_tex,
					       DXGI_RESOURCE_PRIORITY_MAXIMUM);

	*km_out = km;

	struct handle_tex new_ht = {handle, input_tex, km};
	da_push_back(enc->input_textures, &new_ht);
	return input_tex;
}

#endif /* defined(_WIN32) */

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize)
{
	void *s = enc->session;

	da_resize(enc->packet_data, 0);

	if (!enc->buffers_queued)
		return true;
	if (!finalize && enc->buffers_queued < enc->output_delay)
		return true;

	size_t count = finalize ? enc->buffers_queued : 1;

	for (size_t i = 0; i < count; i++) {
		size_t cur_bs_idx = enc->cur_bitstream;
		struct nv_bitstream *bs = &enc->bitstreams.array[cur_bs_idx];
		struct nv_texture *nvtex = &enc->textures.array[cur_bs_idx];

		/* ---------------- */

		NV_ENC_LOCK_BITSTREAM lock = {NV_ENC_LOCK_BITSTREAM_VER};
		lock.outputBitstream = bs->ptr;
		lock.doNotWait = false;

		if (NV_FAILED(nv.nvEncLockBitstream(s, &lock))) {
			return false;
		}

		if (enc->first_packet) {
			uint8_t *new_packet;
			size_t size;

			enc->first_packet = false;
			obs_extract_avc_headers(lock.bitstreamBufferPtr,
						lock.bitstreamSizeInBytes,
						&new_packet, &size,
						&enc->header, &enc->header_size,
						&enc->sei, &enc->sei_size);

			da_copy_array(enc->packet_data, new_packet, size);
			bfree(new_packet);
		} else {
			da_copy_array(enc->packet_data, lock.bitstreamBufferPtr,
				      lock.bitstreamSizeInBytes);
		}

		enc->packet_pts = (int64_t)lock.outputTimeStamp;
		enc->packet_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;

		if (NV_FAILED(nv.nvEncUnlockBitstream(s, bs->ptr))) {
			return false;
		}

		/* ---------------- */

		if (nvtex->mapped_res) {
			NVENCSTATUS err;
			err = nv.nvEncUnmapInputResource(s, nvtex->mapped_res);
			if (nv_failed(enc->encoder, err, __FUNCTION__,
				      "unmap")) {
				return false;
			}
			nvtex->mapped_res = NULL;
		}

		/* ---------------- */

		if (++enc->cur_bitstream == enc->buf_count)
			enc->cur_bitstream = 0;

		enc->buffers_queued--;
	}

	return true;
}

static bool nvenc_encode_tex(void *data, struct encoder_texture *tex,
			     int64_t pts, uint64_t lock_key, uint64_t *next_key,
			     struct encoder_packet *packet,
			     bool *received_packet)
{
	struct nvenc_data *enc = data;
#ifdef _WIN32
	ID3D11Device *device = enc->device;
	ID3D11DeviceContext *context = enc->context;
	ID3D11Texture2D *input_tex;
	ID3D11Texture2D *output_tex;
	IDXGIKeyedMutex *km;
#else
	GLuint input_tex[2];
	GLuint output_tex;
#endif
	struct nv_texture *nvtex;
	struct nv_bitstream *bs;
	NVENCSTATUS err;
	bool use_nv12 = tex->info.format == VIDEO_FORMAT_NV12;

#ifdef _WIN32
	if (tex == NULL || tex->handle == GS_INVALID_HANDLE) {
#else
	if (tex == NULL || tex->tex[0] == NULL) {
#endif
		error("Encode failed: bad texture handle");
		*next_key = lock_key;
		return false;
	}

	bs = &enc->bitstreams.array[enc->next_bitstream];
	nvtex = &enc->textures.array[enc->next_bitstream];

#ifndef _WIN32
	obs_enter_graphics();
#endif

	if (!complete_texture(enc, nvtex, use_nv12)) {
		error("Encode failed: could not complete texture");
		*next_key = lock_key;
		goto error;
	}

#ifdef _WIN32
	input_tex = get_tex_from_handle(enc, tex->handle, &km);
	if (!input_tex) {
		*next_key = lock_key;
		return false;
	}
#else
	input_tex[0] = *(GLuint *)gs_texture_get_obj(tex->tex[0]);
	if (use_nv12) {
		input_tex[1] = *(GLuint *)gs_texture_get_obj(tex->tex[1]);
	}
#endif
	output_tex = nvtex->tex;

	circlebuf_push_back(&enc->dts_list, &pts, sizeof(pts));

	/* ------------------------------------ */
	/* wait for output bitstream/tex        */

#ifdef _WIN32
	WaitForSingleObject(bs->event, INFINITE);
#endif

	/* ------------------------------------ */
	/* copy to output tex                   */

#ifdef _WIN32

	km->lpVtbl->AcquireSync(km, lock_key, INFINITE);

	context->lpVtbl->CopyResource(context, (ID3D11Resource *)output_tex,
				      (ID3D11Resource *)input_tex);

	km->lpVtbl->ReleaseSync(km, *next_key);

#else /* defined(_WIN32) */

	glCopyImageSubData(input_tex[0], GL_TEXTURE_2D, 0, 0, 0, 0, output_tex,
			   GL_TEXTURE_2D, 0, 0, 0, 0, enc->cx, enc->cy, 1);
	if (gl_error()) {
		goto error;
	}

	/*if (use_nv12) {
		glCopyImageSubData(input_tex[1], GL_TEXTURE_2D, 0, 0, 0, 0,
				   output_tex, GL_TEXTURE_2D, 0, 0, enc->cy, 0,
				   enc->cx / 2, enc->cy / 2, 1);
		if (gl_error()) {
			goto error;
		}
	}*/

#endif /* defined(_WIN32) */

	/* ------------------------------------ */
	/* map output tex so nvenc can use it   */

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER, 0};
	map.registeredResource = nvtex->res;
	if (NV_FAILED(nv.nvEncMapInputResource(enc->session, &map))) {
		goto error;
	}

	nvtex->mapped_res = map.mappedResource;

	/* ------------------------------------ */
	/* do actual encode call                */

	NV_ENC_PIC_PARAMS params = {0};
	params.version = NV_ENC_PIC_PARAMS_VER;
	params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	params.inputBuffer = map.mappedResource;
	params.bufferFmt = map.mappedBufferFmt;
	params.inputTimeStamp = (uint64_t)pts;
	params.inputWidth = enc->cx;
	params.inputHeight = enc->cy;
	params.outputBitstream = bs->ptr;
#ifdef _WIN32
	params.completionEvent = bs->event;
#endif

	err = nv.nvEncEncodePicture(enc->session, &params);
	if (err != NV_ENC_SUCCESS && err != NV_ENC_ERR_NEED_MORE_INPUT) {
		nv_failed(enc->encoder, err, __FUNCTION__,
			  "nvEncEncodePicture");
		goto error;
	}

	enc->encode_started = true;
	enc->buffers_queued++;

	if (++enc->next_bitstream == enc->buf_count) {
		enc->next_bitstream = 0;
	}

	/* ------------------------------------ */
	/* check for encoded packet and parse   */

	if (!get_encoded_packet(enc, false)) {
		goto error;
	}

#ifndef _WIN32
	obs_leave_graphics();
#endif

	/* ------------------------------------ */
	/* output encoded packet                */

	if (enc->packet_data.num) {
		int64_t dts;
		circlebuf_pop_front(&enc->dts_list, &dts, sizeof(dts));

		/* subtract bframe delay from dts */
		dts -= (int64_t)enc->bframes * packet->timebase_num;

		*received_packet = true;
		packet->data = enc->packet_data.array;
		packet->size = enc->packet_data.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->pts = enc->packet_pts;
		packet->dts = dts;
		packet->keyframe = enc->packet_keyframe;
	} else {
		*received_packet = false;
	}

	return true;

error:
#ifndef _WIN32
	obs_leave_graphics();
#endif
	return false;
}

static bool nvenc_encode_texture_available(void *data,
					   struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	return is_format_supported(info->format);
}

extern void nvenc_defaults(obs_data_t *settings);
extern obs_properties_t *nvenc_properties(void *unused);

static bool nvenc_extra_data(void *data, uint8_t **header, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->header) {
		return false;
	}

	*header = enc->header;
	*size = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **sei, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->sei) {
		return false;
	}

	*sei = enc->sei;
	*size = enc->sei_size;
	return true;
}

struct obs_encoder_info nvenc_info = {
	.id = "jim_nvenc",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = nvenc_get_name,
	.create = nvenc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode_texture_available = nvenc_encode_texture_available,
	.encode_texture2 = nvenc_encode_tex,
	.get_defaults = nvenc_defaults,
	.get_properties = nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
};
