// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause

#include "ara_runtime.hpp"

#include <dlfcn.h>
#include <cstdio>

#include <glib.h>

// ---------------------------------------------------------------------------
// AraModel
// ---------------------------------------------------------------------------
AraModel::AraModel(AraRuntime *rt, dv_model_t *model)
    : mRt(rt), mModel(model) {
  if (mModel) {
    mNumInputs = mModel->num_inputs;
    mNumOutputs = mModel->num_outputs;
  }
}

AraModel::~AraModel() {
  if (mModel && mRt && mRt->pModelUnload) {
    mRt->pModelUnload(mModel);
  }
  mModel = nullptr;
}

const dv_model_input_param_t *AraModel::inputParam(int i) const {
  if (!mModel || i < 0 || i >= mNumInputs) return nullptr;
  return &mModel->input_param[i];
}

const dv_model_output_param_t *AraModel::outputParam(int i) const {
  if (!mModel || i < 0 || i >= mNumOutputs) return nullptr;
  return &mModel->output_param[i];
}

bool AraModel::infer(const std::vector<AraBlob> &inputs,
                     const std::vector<AraBlob> &outputs) {
  if (!mRt || !mRt->pInferSync || !mModel) return false;
  if (inputs.empty() || outputs.empty()) return false;

  // Build raw-pointer blob lists.
  std::vector<dv_blob_t> ip(inputs.size());
  std::vector<dv_blob_t> op(outputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    ip[i] = {};
    ip[i].handle = inputs[i].data;
    ip[i].size = inputs[i].size;
    ip[i].blob_type = DV_BLOB_TYPE_RAW_POINTER;
  }
  for (size_t i = 0; i < outputs.size(); ++i) {
    op[i] = {};
    op[i].handle = outputs[i].data;
    op[i].size = outputs[i].size;
    op[i].blob_type = DV_BLOB_TYPE_RAW_POINTER;
  }

  dv_infer_request_t *req = nullptr;
  dv_status_code_t st = mRt->pInferSync(mRt->mSession, mRt->mEndpoints, mModel,
                                        ip.data(), op.data(),
                                        /*timeout_ms*/ 50000,
                                        /*enable_stats*/ false, &req);
  if (req && mRt->pInferFree) mRt->pInferFree(req);
  if (st != DV_SUCCESS) {
    g_printerr("[ARA] dv_infer_sync failed: %d (%s)\n", st,
               mRt->pStringify ? mRt->pStringify(st) : "?");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// AraRuntime
// ---------------------------------------------------------------------------
AraRuntime::~AraRuntime() {
  if (mSession && pSessionClose) pSessionClose(mSession);
  mSession = nullptr;
  if (mLib) dlclose(mLib);
  mLib = nullptr;
  mInitialized = false;
}

bool AraRuntime::check(const char *what, dv_status_code_t st) {
  if (st == DV_SUCCESS) return true;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s failed: %d (%s)", what, st,
           pStringify ? pStringify(st) : "?");
  mLastError = buf;
  g_printerr("[ARA] %s\n", buf);
  return false;
}

bool AraRuntime::loadSymbols() {
#define ARA_SYM(field, type, name)                                    \
  do {                                                                \
    field = reinterpret_cast<type>(dlsym(mLib, name));                \
    if (!field) {                                                     \
      mLastError = std::string("dvapi symbol not found: ") + name;    \
      g_printerr("[ARA] %s\n", mLastError.c_str());                   \
      return false;                                                   \
    }                                                                 \
  } while (0)

  ARA_SYM(pStringify, fn_stringify, "dv_stringify_status_code");
  ARA_SYM(pSetLogLevel, fn_set_log_level, "dv_client_set_log_level");
  ARA_SYM(pSessionUnix, fn_session_unix, "dv_session_create_via_unix_socket");
  ARA_SYM(pSessionClose, fn_session_close, "dv_session_close");
  ARA_SYM(pEndpointGetList, fn_endpoint_get_list, "dv_endpoint_get_list");
  ARA_SYM(pModelLoad, fn_model_load, "dv_model_load_from_file");
  ARA_SYM(pModelUnload, fn_model_unload, "dv_model_unload");
  ARA_SYM(pInferSync, fn_infer_sync, "dv_infer_sync");
  ARA_SYM(pInferFree, fn_infer_free, "dv_infer_free");
#undef ARA_SYM
  return true;
}

bool AraRuntime::init(const std::string &socketPath) {
  if (mInitialized) return true;

  mLib = dlopen("libaraclient_aarch64.so", RTLD_NOW | RTLD_LOCAL);
  if (!mLib) mLib = dlopen("/usr/lib/libaraclient_aarch64.so", RTLD_NOW | RTLD_LOCAL);
  if (!mLib) {
    mLastError = std::string("dlopen libaraclient_aarch64.so failed: ") + dlerror();
    g_printerr("[ARA] %s\n", mLastError.c_str());
    return false;
  }

  if (!loadSymbols()) return false;

  pSetLogLevel(DV_CLIENT_LOG_LEVEL_ERROR);

  if (!check("dv_session_create_via_unix_socket",
             pSessionUnix(socketPath.c_str(), &mSession)))
    return false;

  if (!check("dv_endpoint_get_list",
             pEndpointGetList(mSession, &mEndpoints, &mEndpointCount)))
    return false;

  if (mEndpointCount <= 0) {
    mLastError = "no Ara endpoint found";
    g_printerr("[ARA] %s\n", mLastError.c_str());
    return false;
  }

  g_print("[ARA] runtime initialized: socket=%s endpoints=%d\n",
          socketPath.c_str(), mEndpointCount);
  mInitialized = true;
  return true;
}

std::unique_ptr<AraModel> AraRuntime::loadModel(const std::string &modelPath,
                                                const std::string &name) {
  if (!mInitialized) {
    g_printerr("[ARA] loadModel called before init\n");
    return nullptr;
  }
  dv_model_t *model = nullptr;
  if (!check("dv_model_load_from_file",
             pModelLoad(mSession, mEndpoints, modelPath.c_str(), name.c_str(),
                        DV_MODEL_PRIORITY_LEVEL_DEFAULT, &model)) ||
      !model) {
    return nullptr;
  }
  g_print("[ARA] loaded model '%s' from %s (num_inputs=%d num_outputs=%d)\n",
          name.c_str(), modelPath.c_str(), model->num_inputs, model->num_outputs);
  return std::unique_ptr<AraModel>(new AraModel(this, model));
}
