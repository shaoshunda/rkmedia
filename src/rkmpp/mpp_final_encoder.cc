// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>

#include "buffer.h"

#include "media_type.h"
#include "mpp_encoder.h"

namespace easymedia {

class MPPConfig {
public:
  MPPConfig();
  virtual ~MPPConfig();
  virtual bool InitConfig(MPPEncoder &mpp_enc, const MediaConfig &cfg) = 0;
  virtual bool CheckConfigChange(MPPEncoder &mpp_enc, uint32_t change,
                                 std::shared_ptr<ParameterBuffer> val) = 0;
  MppEncCfg enc_cfg;
};

MPPConfig::MPPConfig() {
  enc_cfg = NULL;
  if (mpp_enc_cfg_init(&enc_cfg)) {
    LOG("ERROR: MPP Encoder: MPPConfig: cfg init failed!");
    enc_cfg = NULL;
    return;
  }
  LOG("MPP Encoder: MPPConfig: cfg init sucess!\n");
}

MPPConfig::~MPPConfig() {
  if (enc_cfg) {
    mpp_enc_cfg_deinit(enc_cfg);
    LOG("MPP Encoder: MPPConfig: cfg deinit done!\n");
  }
}

class MPPMJPEGConfig : public MPPConfig {
public:
  MPPMJPEGConfig() {}
  ~MPPMJPEGConfig() = default;
  virtual bool InitConfig(MPPEncoder &mpp_enc, const MediaConfig &cfg) override;
  virtual bool CheckConfigChange(MPPEncoder &mpp_enc, uint32_t change,
                                 std::shared_ptr<ParameterBuffer> val) override;
};

bool MPPMJPEGConfig::InitConfig(MPPEncoder &mpp_enc, const MediaConfig &cfg) {
  const ImageConfig &img_cfg = cfg.img_cfg;
  const ImageInfo &image_info = cfg.img_cfg.image_info;
  MppPollType timeout = MPP_POLL_BLOCK;
  MppFrameFormat pic_type = ConvertToMppPixFmt(image_info.pix_fmt);
  int line_size = image_info.vir_width;

  if (!enc_cfg) {
    LOG("ERROR: MPP Encoder[JPEG]: mpp enc cfg is null!\n");
    return false;
  }

  // check param
  VALUE_SCOPE_CHECK(img_cfg.qp_init, 1, 10);
  if (pic_type == -1) {
    LOG("ERROR: MPP Encoder[JPEG]: invalid pixel format\n");
    return false;
  }

  LOG("MPP Encoder[JPEG]: Set output block mode.\n");
  int ret = mpp_enc.EncodeControl(MPP_SET_OUTPUT_TIMEOUT, &timeout);
  if (ret != 0) {
    LOG("ERROR: MPP Encoder[JPEG]: set output block failed! ret %d\n", ret);
    return false;
  }
  LOG("MPP Encoder[JPEG]: Set input block mode.\n");
  ret = mpp_enc.EncodeControl(MPP_SET_INPUT_TIMEOUT, &timeout);
  if (ret != 0) {
    LOG("ERROR: MPP Encoder[JPEG]: set input block failed! ret %d\n", ret);
    return false;
  }

  mpp_enc.GetConfig().img_cfg.image_info = image_info;
  mpp_enc.GetConfig().type = Type::Image;

  if (pic_type == MPP_FMT_YUV422_YUYV || pic_type == MPP_FMT_YUV422_UYVY)
    line_size *= 2;

  // precfg set.
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:width", image_info.width);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:height", image_info.height);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", line_size);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", image_info.vir_height);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:format", pic_type);
  // quant set.
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "jpeg:quant", img_cfg.qp_init);
  if (ret) {
    LOG("ERROR: MPP Encoder[JPEG]: cfg set s32 failed ret %d\n", ret);
    return false;
  }

  ret = mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg);
  if (ret) {
    LOG("ERROR: MPP Encoder[JPEG]: encoder set cfg failed! ret=%d\n", ret);
    return false;
  }

  LOG("MPP Encoder[JPEG]: w x h(%d[%d] x %d[%d])\n", image_info.width,
    line_size, image_info.height, image_info.vir_height);

  return true;
}

bool MPPMJPEGConfig::CheckConfigChange(MPPEncoder &mpp_enc, uint32_t change,
                                       std::shared_ptr<ParameterBuffer> val) {
  ImageConfig &iconfig = mpp_enc.GetConfig().img_cfg;

  if (!enc_cfg) {
    LOG("ERROR: MPP Encoder[JPEG]: mpp enc cfg is null!\n");
    return false;
  }

  if (change & VideoEncoder::kQPChange) {
    int quant = val->GetValue();
    VALUE_SCOPE_CHECK(quant, 1, 10);
    int ret = mpp_enc_cfg_set_s32(enc_cfg, "jpeg:quant", quant);
    if (ret) {
      LOG("ERROR: MPP Encoder[JPEG]: cfg set s32 failed! ret=%d\n", ret);
      return false;
    }

    ret = mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg);
    if (ret) {
      LOG("ERROR: MPP Encoder[JPEG]: set cfg failed! ret=%d\n", ret);
      return false;
    }

    LOG("MPP Encoder[JPEG]: quant = %d\n", quant);
    iconfig.qp_init = quant;
  } else {
    LOG("MPP Encoder[JPEG]: Unsupport request change 0x%08x!\n", change);
    return false;
  }

  return true;
}

class MPPCommonConfig : public MPPConfig {
public:
  static const int kMPPMinBps = 2 * 1000;
  static const int kMPPMaxBps = 98 * 1000 * 1000;

  MPPCommonConfig(MppCodingType type) : code_type(type) {}
  ~MPPCommonConfig() = default;
  virtual bool InitConfig(MPPEncoder &mpp_enc, const MediaConfig &cfg) override;
  virtual bool CheckConfigChange(MPPEncoder &mpp_enc, uint32_t change,
                                 std::shared_ptr<ParameterBuffer> val) override;

private:
  MppCodingType code_type;
};

// According to bps_max, automatically calculate bps_target and bps_min.
static int CalcMppBpsWithMax(MppEncRcMode rc_mode,
  int &bps_max, int &bps_min, int &bps_target) {
  if ((bps_max > MPPCommonConfig::kMPPMaxBps) ||
    (bps_max < MPPCommonConfig::kMPPMinBps)) {
    LOG("ERROR: MPP Encoder: bps <%d> is not valid!\n", bps_max);
    return -1;
  }

  switch (rc_mode) {
  case MPP_ENC_RC_MODE_CBR:
    // constant bitrate has very small bps range of 1/16 bps
    bps_target = bps_max * 16 / 17;
    bps_min = bps_max * 15 / 17;
    break;
  case MPP_ENC_RC_MODE_VBR:
    // variable bitrate has large bps range
    bps_target = bps_max * 2 / 3;
    bps_min = bps_max * 1 / 3;
    break;
  default:
    // TODO
    LOG("right now rc_mode=%d is untested\n", rc_mode);
    return -1;
  }

  if (bps_min < MPPCommonConfig::kMPPMinBps)
    bps_min = MPPCommonConfig::kMPPMinBps;
  if (bps_target < bps_min)
    bps_target = (bps_min + bps_max) / 2;

  return 0;
}

bool MPPCommonConfig::InitConfig(MPPEncoder &mpp_enc, const MediaConfig &cfg) {
  VideoConfig vconfig = cfg.vid_cfg;
  const ImageConfig &img_cfg = vconfig.image_cfg;
  const ImageInfo &image_info = cfg.img_cfg.image_info;
  MppPollType timeout = MPP_POLL_BLOCK;
  MppFrameFormat pic_type = ConvertToMppPixFmt(image_info.pix_fmt);
  int line_size = image_info.vir_width;

  if (!enc_cfg) {
    LOG("ERROR: MPP Encoder: mpp enc cfg is null!\n");
    return false;
  }

  //Encoder param check.
  LOG("MPP Encoder: Checking encoder config....\n");
  VALUE_SCOPE_CHECK(vconfig.frame_rate, 1, 60);
  VALUE_SCOPE_CHECK(vconfig.gop_size, 0, 0x7FFFFFFF);
  VALUE_SCOPE_CHECK(vconfig.qp_max, 8, 51);
  VALUE_SCOPE_CHECK(vconfig.qp_min, 1, VALUE_MIN(vconfig.qp_max, 48));
  VALUE_SCOPE_CHECK(img_cfg.qp_init, vconfig.qp_min, vconfig.qp_max);
  VALUE_SCOPE_CHECK(vconfig.qp_step, 0, (vconfig.qp_max - vconfig.qp_min));
  VALUE_SCOPE_CHECK(img_cfg.image_info.vir_width, 1, 8192);
  VALUE_SCOPE_CHECK(img_cfg.image_info.vir_height, 1, 8192);
  VALUE_SCOPE_CHECK(img_cfg.image_info.width, 1,
    img_cfg.image_info.vir_width);
  VALUE_SCOPE_CHECK(img_cfg.image_info.height, 1,
    img_cfg.image_info.vir_height);
  if ((vconfig.max_i_qp > 0) || (vconfig.min_i_qp > 0)) {
    VALUE_SCOPE_CHECK(vconfig.max_i_qp, 8, 51);
    VALUE_SCOPE_CHECK(vconfig.min_i_qp, 1, VALUE_MIN(vconfig.max_i_qp, 48));
  }

  if (pic_type == -1) {
    LOG("error input pixel format\n");
    return false;
  }

  MppEncRcMode rc_mode = GetMPPRCMode(vconfig.rc_mode);
  if (rc_mode == MPP_ENC_RC_MODE_BUTT) {
    LOG("ERROR: MPP Encoder: Invalid rc mode %s\n", vconfig.rc_mode);
    return false;
  }
  int bps_max = vconfig.bit_rate;
  int bps_min = bps_max;
  int bps_target = bps_max;
  int fps_in_num = std::max(1, std::min(vconfig.frame_rate, (1 << 16) - 1));
  int fps_in_den = 1;
  int fps_out_num = fps_in_num;
  int fps_out_den = 1;
  int gop = vconfig.gop_size;
  int full_range = 1; // default enable full range.

  if (CalcMppBpsWithMax(rc_mode, bps_max, bps_min, bps_target) < 0)
    return false;

  if (pic_type == MPP_FMT_YUV422_YUYV || pic_type == MPP_FMT_YUV422_UYVY)
    line_size *= 2;

  LOG("MPP Encoder: Set output block mode.\n");
  int ret = mpp_enc.EncodeControl(MPP_SET_OUTPUT_TIMEOUT, &timeout);
  if (ret != 0) {
    LOG("ERROR: MPP Encoder: set output block failed ret %d\n", ret);
    return false;
  }
  LOG("MPP Encoder: Set input block mode.\n");
  ret = mpp_enc.EncodeControl(MPP_SET_INPUT_TIMEOUT, &timeout);
  if (ret != 0) {
    LOG("ERROR: MPP Encoder: set input block failed ret %d\n", ret);
    return false;
  }

  // precfg set.
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:width", image_info.width);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:height", image_info.height);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", line_size);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", image_info.vir_height);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:format", pic_type);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "prep:range", full_range);

  // rccfg set.
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", rc_mode);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", bps_min);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", bps_max);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", bps_target);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_flex", 0);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", fps_in_num);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denorm", fps_in_den);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_flex", 0);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", fps_out_num);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denorm", fps_out_den);
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", gop);

  vconfig.frame_rate = fps_in_num;
  LOG("MPP Encoder: bps:[%d,%d,%d] fps: [%d/%d]->[%d/%d], gop:%d\n",
    bps_max, bps_target, bps_min, fps_in_num, fps_in_den,
    fps_out_num, fps_out_den, gop);

  // codeccfg set.
  ret |= mpp_enc_cfg_set_s32(enc_cfg, "codec:type", code_type);
  switch (code_type) {
  case MPP_VIDEO_CodingAVC:
    // H.264 profile_idc parameter
    // 66  - Baseline profile
    // 77  - Main profile
    // 100 - High profile
    if (vconfig.profile != 66 && vconfig.profile != 77)
      vconfig.profile = 100; // default PROFILE_HIGH 100
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:profile", vconfig.profile);

    // H.264 level_idc parameter
    // 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
    // 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
    // 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
    // 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
    // 50 / 51 / 52         - 4K@30fps
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:level", vconfig.level);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:cabac_en",
      (vconfig.profile == 100) ? 1 : 0);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:cabac_idc", 0);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:trans8x8",
      (vconfig.trans_8x8 && (vconfig.profile == 100)) ? 1 : 0);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_init",
      (rc_mode == MPP_ENC_RC_MODE_FIXQP) ? -1 : img_cfg.qp_init);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max", vconfig.qp_max);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min", vconfig.qp_min);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_step", vconfig.qp_step);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max_i", vconfig.max_i_qp);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min_i", vconfig.min_i_qp);
    LOG("MPP Encoder: AVC: encode profile %d level %d init_qp %d\n",
      vconfig.profile, vconfig.level, img_cfg.qp_init);
    break;
  case MPP_VIDEO_CodingHEVC:
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_init",
      (rc_mode == MPP_ENC_RC_MODE_FIXQP) ? -1 : img_cfg.qp_init);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max", vconfig.qp_max);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min", vconfig.qp_min);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_step", vconfig.qp_step);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max_i", vconfig.max_i_qp);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min_i", vconfig.min_i_qp);
    break;
  default:
    // will never go here, avoid gcc warning
    return false;
  }

  if (ret) {
    LOG("ERROR: MPP Encoder: cfg set s32 failed ret %d\n", ret);
    return false;
  }

  ret = mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg);
  if (ret) {
    LOG("ERROR: MPP Encoder: set cfg failed ret %d\n", ret);
    return false;
  }

  LOG("MPP Encoder: w x h(%d[%d] x %d[%d])\n", image_info.width,
    line_size, image_info.height, image_info.vir_height);

#if 0
  MppPacket packet = nullptr;
  ret = mpp_enc.EncodeControl(MPP_ENC_GET_EXTRA_INFO, &packet);
  if (ret) {
    LOG("ERROR: MPP Encoder: get extra info failed\n");
    return false;
  }

  // Get and write sps/pps for H.264/5
  if (packet) {
    void *ptr = mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);
    if (!mpp_enc.SetExtraData(ptr, len)) {
      LOG("ERROR: MPP Encoder: SetExtraData failed\n");
      return false;
    }
    mpp_enc.GetExtraData()->SetUserFlag(MediaBuffer::kExtraIntra);
    packet = NULL;
  }
#endif

  int header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
  ret = mpp_enc.EncodeControl(MPP_ENC_SET_HEADER_MODE, &header_mode);
  if (ret) {
    LOG("ERROR: MPP Encoder: set heder mode failed ret %d\n", ret);
    return false;
  }

  mpp_enc.GetConfig().vid_cfg = vconfig;
  mpp_enc.GetConfig().type = Type::Video;
  return true;
}

bool MPPCommonConfig::CheckConfigChange(MPPEncoder &mpp_enc, uint32_t change,
                                        std::shared_ptr<ParameterBuffer> val) {
  VideoConfig &vconfig = mpp_enc.GetConfig().vid_cfg;
  int ret = 0;

  if (!enc_cfg) {
    LOG("ERROR: MPP Encoder: mpp enc cfg is null!\n");
    return false;
  }

  if (change & VideoEncoder::kFrameRateChange) {
    uint8_t *values = (uint8_t *)val->GetPtr();
    if (val->GetSize() < 4) {
      LOG("ERROR: MPP Encoder: fps should be array[4Byte]:"
        "{inFpsNum, inFpsDen, outFpsNum, outFpsDen}");
      return false;
    }
    uint8_t in_fps_num = values[0];
    uint8_t in_fps_den = values[1];
    uint8_t out_fps_num = values[2];
    uint8_t out_fps_den = values[3];

    if (!out_fps_num || !out_fps_den || (out_fps_num > 60)) {
      LOG("ERROR: MPP Encoder: invalid out fps: [%d/%d]\n",
        out_fps_num, out_fps_den);
      return false;
    }

    if (in_fps_num && in_fps_den) {
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", in_fps_num);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denorm", in_fps_num);
    }
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", out_fps_num);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denorm", out_fps_den);
    if (ret) {
      LOG("ERROR: MPP Encoder: fps: cfg set s32 failed ret %d\n", ret);
      return false;
    }
    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: change fps cfg failed!\n");
      return false;
    }
    if (in_fps_num && in_fps_den) {
      LOG("MPP Encoder: new fps: [%d/%d]->[%d/%d]\n",
          in_fps_num, in_fps_den, out_fps_num, out_fps_den);
    } else
      LOG("MPP Encoder: new out fps: [%d/%d]\n", out_fps_num, out_fps_den);

    vconfig.frame_rate = out_fps_num;
  } else if (change & VideoEncoder::kBitRateChange) {
    int bps_max = val->GetValue();
    int bps_min = bps_max;
    int bps_target = bps_max;
    MppEncRcMode rc_mode = GetMPPRCMode(vconfig.rc_mode);
    if (rc_mode == MPP_ENC_RC_MODE_BUTT) {
      LOG("ERROR: MPP Encoder: bps: invalid rc mode %s\n", vconfig.rc_mode);
      return false;
    }
    LOG("MPP Encoder: new bpsmax:%d\n", bps_max);
    if (CalcMppBpsWithMax(rc_mode, bps_max, bps_min, bps_target) < 0)
      return false;

    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", bps_min);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", bps_max);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", bps_target);
    if (ret) {
      LOG("ERROR: MPP Encoder: bps: cfg set s32 failed ret %d\n", ret);
      return false;
    }

    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: change bps cfg failed!\n");
      return false;
    }

    vconfig.bit_rate = bps_max;
  } else if (change & VideoEncoder::kRcModeChange) {
    char *new_mode = (char *)val->GetPtr();
    LOG("MPP Encoder: new rc_mode:%s\n", new_mode);
    MppEncRcMode rc_mode = GetMPPRCMode(new_mode);
    if (rc_mode == MPP_ENC_RC_MODE_BUTT) {
      LOG("ERROR: MPP Encoder: rc_mode is invalid! should be cbr/vbr.\n");
      return false;
    }

    //Recalculate bps
    int bps_max = vconfig.bit_rate;
    int bps_min = bps_max;
    int bps_target = bps_max;
    if (CalcMppBpsWithMax(rc_mode, bps_max, bps_min, bps_target) < 0)
      return false;

    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", rc_mode);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", bps_min);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", bps_max);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", bps_target);
    if (ret) {
      LOG("ERROR: MPP Encoder: rc mode: cfg set s32 failed ret %d\n", ret);
      return false;
    }

    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: change rc_mode cfg failed!\n");
      return false;
    }
    // save new value to encoder->vconfig.
    if (rc_mode == MPP_ENC_RC_MODE_VBR)
      vconfig.rc_mode = KEY_VBR;
    else
      vconfig.rc_mode = KEY_CBR;
  } else if (change & VideoEncoder::kRcQualityChange) {
    LOG("WARN: MPP Encoder: rc_quality is deprecated!\n");
  } else if (change & VideoEncoder::kGopChange) {
    int new_gop_size = val->GetValue();
    if(new_gop_size < 0) {
      LOG("ERROR: MPP Encoder: gop size invalid!\n");
      return false;
    }
    LOG("MPP Encoder: gop change frome %d to %d\n", vconfig.gop_size, new_gop_size);
    ret |= mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", new_gop_size);
    if (ret) {
      LOG("ERROR: MPP Encoder: gop: cfg set s32 failed ret %d\n", ret);
      return false;
    }
    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: change gop cfg failed!\n");
      return false;
    }
    //save to vconfig
    vconfig.gop_size = new_gop_size;
  } else if (change & VideoEncoder::kQPChange) {
    VideoEncoderQp *qps = (VideoEncoderQp *)val->GetPtr();
    if (val->GetSize() < sizeof(VideoEncoderQp)) {
      LOG("ERROR: MPP Encoder: Incomplete VideoEncoderQp information\n");
      return false;
    }
    LOG("MPP Encoder: new qp:[%d, %d, %d, %d, %d, %d]\n",
      qps->qp_init, qps->qp_step, qps->qp_min, qps->qp_max,
      qps->min_i_qp, qps->max_i_qp);

    if (code_type == MPP_VIDEO_CodingAVC) {
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_init", qps->qp_init);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max", qps->qp_max);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min", qps->qp_min);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_step", qps->qp_step);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max_i", qps->max_i_qp);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min_i", qps->min_i_qp);
    } else if (code_type == MPP_VIDEO_CodingHEVC) {
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_init", qps->qp_init);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max", qps->qp_max);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min", qps->qp_min);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_step", qps->qp_step);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max_i", qps->max_i_qp);
      ret |= mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min_i", qps->min_i_qp);
    }
    if (ret) {
      LOG("ERROR: MPP Encoder: qp: cfg set s32 failed ret %d\n", ret);
      return false;
    }

    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: change qp cfg failed!\n");
      return false;
    }
    vconfig.image_cfg.qp_init = qps->qp_init;
    vconfig.qp_min = qps->qp_min;
    vconfig.qp_max = qps->qp_max;
    vconfig.qp_step = qps->qp_step;
    vconfig.max_i_qp = qps->max_i_qp;
    vconfig.min_i_qp = qps->min_i_qp;
  } else if (change & VideoEncoder::kROICfgChange) {
    EncROIRegion *regions = (EncROIRegion *)val->GetPtr();
    if (val->GetSize() && (val->GetSize() < sizeof(EncROIRegion))) {
      LOG("ERROR: MPP Encoder: ParameterBuffer size is invalid!\n");
      return false;
    }
    int region_cnt = val->GetSize() / sizeof(EncROIRegion);
    mpp_enc.RoiUpdateRegions(regions, region_cnt);
  } else if (change & VideoEncoder::kForceIdrFrame) {
    LOG("MPP Encoder: force idr frame...\n");
    if (mpp_enc.EncodeControl(MPP_ENC_SET_IDR_FRAME, nullptr) != 0) {
      LOG("ERROR: MPP Encoder: force idr frame control failed!\n");
      return false;
    }
  } else if (change & VideoEncoder::kSplitChange) {
    if (val->GetSize() < (2 * sizeof(int))) {
      LOG("ERROR: MPP Encoder: Incomplete split information\n");
      return false;
    }
    RK_U32 split_mode = *((unsigned int *)val->GetPtr());
    RK_U32 split_arg = *((unsigned int *)val->GetPtr() + 1);

    LOG("MPP Encoder: split_mode:%u, split_arg:%u\n", split_mode, split_arg);
    ret |= mpp_enc_cfg_set_u32(enc_cfg, "split:mode", split_mode);
    ret |= mpp_enc_cfg_set_u32(enc_cfg, "split:arg", split_arg);
    if (ret) {
      LOG("ERROR: MPP Encoder: split: cfg set s32 failed ret %d\n", ret);
      return false;
    }

    if (mpp_enc.EncodeControl(MPP_ENC_SET_CFG, enc_cfg) != 0) {
      LOG("ERROR: MPP Encoder: set split mode failed!\n");
      return false;
    }
  }
#ifdef MPP_SUPPORT_HW_OSD
  else if (change & VideoEncoder::kOSDDataChange) {
    // type: OsdRegionData*
    LOGD("MPP Encoder: config osd regions\n");
    if (val->GetSize() < sizeof(OsdRegionData)) {
      LOG("ERROR: MPP Encoder: palette buff should be OsdRegionData type\n");
      return false;
    }
    OsdRegionData *param = (OsdRegionData *)val->GetPtr();
    if (mpp_enc.OsdRegionSet(param)) {
      LOG("ERROR: MPP Encoder: set osd regions error!\n");
      return false;
    }
  } else if (change & VideoEncoder::kOSDPltChange) {
    // type: 265 * U32 array.
    LOG("MPP Encoder: config osd palette\n");
    if (val->GetSize() < (sizeof(int) * 4)) {
      LOG("ERROR: MPP Encoder: palette buff should be U32 * 256\n");
      return false;
    }
    uint32_t *param = (uint32_t *)val->GetPtr();
    if (mpp_enc.OsdPaletteSet(param)) {
      LOG("ERROR: MPP Encoder: set Palette error!\n");
      return false;
    }
  }
#endif
  else {
    LOG("Unsupport request change 0x%08x!\n", change);
    return false;
  }

  return true;
}

class MPPFinalEncoder : public MPPEncoder {
public:
  MPPFinalEncoder(const char *param);
  virtual ~MPPFinalEncoder() {
    if (mpp_config)
      delete mpp_config;
  }

  static const char *GetCodecName() { return "rkmpp"; }
  virtual bool InitConfig(const MediaConfig &cfg) override;

protected:
  // Change configs which are not contained in sps/pps.
  virtual bool CheckConfigChange(
      std::pair<uint32_t, std::shared_ptr<ParameterBuffer>>) override;

  MPPConfig *mpp_config;
};

MPPFinalEncoder::MPPFinalEncoder(const char *param) : mpp_config(nullptr) {
  std::string output_data_type =
      get_media_value_by_key(param, KEY_OUTPUTDATATYPE);
  SetMppCodeingType(output_data_type.empty()
                        ? MPP_VIDEO_CodingUnused
                        : GetMPPCodingType(output_data_type));
}

bool MPPFinalEncoder::InitConfig(const MediaConfig &cfg) {
  assert(!mpp_config);
  MediaConfig new_cfg = cfg;
  switch (coding_type) {
  case MPP_VIDEO_CodingMJPEG:
    mpp_config = new MPPMJPEGConfig();
    new_cfg.img_cfg.codec_type = codec_type;
    break;
  case MPP_VIDEO_CodingAVC:
  case MPP_VIDEO_CodingHEVC:
    new_cfg.vid_cfg.image_cfg.codec_type = codec_type;
    mpp_config = new MPPCommonConfig(coding_type);
    break;
  default:
    LOG("Unsupport mpp encode type: %d\n", coding_type);
    return false;
  }
  if (!mpp_config) {
    LOG_NO_MEMORY();
    return false;
  }
  return mpp_config->InitConfig(*this, new_cfg);
}

bool MPPFinalEncoder::CheckConfigChange(
    std::pair<uint32_t, std::shared_ptr<ParameterBuffer>> change_pair) {
  //common ConfigChange process
  if (change_pair.first & VideoEncoder::kEnableStatistics) {
    bool value = (change_pair.second->GetValue())?true:false;
    set_statistics_switch(value);
    return true;
  }

  assert(mpp_config);
  if (!mpp_config)
    return false;
  return mpp_config->CheckConfigChange(*this, change_pair.first,
                                       change_pair.second);
}

DEFINE_VIDEO_ENCODER_FACTORY(MPPFinalEncoder)
const char *FACTORY(MPPFinalEncoder)::ExpectedInputDataType() {
  return MppAcceptImageFmts();
}

#define IMAGE_JPEG "image:jpeg"
#define VIDEO_H264 "video:h264"
#define VIDEO_H265 "video:h265"

#define VIDEO_ENC_OUTPUT                     \
  TYPENEAR(IMAGE_JPEG) TYPENEAR(VIDEO_H264)  \
  TYPENEAR(VIDEO_H265)

const char *FACTORY(MPPFinalEncoder)::OutPutDataType() { return VIDEO_ENC_OUTPUT; }

} // namespace easymedia
