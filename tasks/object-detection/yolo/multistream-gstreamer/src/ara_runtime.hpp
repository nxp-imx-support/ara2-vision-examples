// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <dvapi.h>  // from RTSDK include dir (already on the include path)

// One inference input/output buffer (raw host pointer + byte size).
struct AraBlob {
  void *data = nullptr;
  size_t size = 0;
};

class AraRuntime;

// A single loaded model on the Ara. Created via AraRuntime::loadModel().
class AraModel {
public:
  ~AraModel();

  bool infer(const std::vector<AraBlob> &inputs,
             const std::vector<AraBlob> &outputs);

  int numInputs() const { return mNumInputs; }
  int numOutputs() const { return mNumOutputs; }
  const dv_model_input_param_t *inputParam(int i) const;
  const dv_model_output_param_t *outputParam(int i) const;

private:
  friend class AraRuntime;
  AraModel(AraRuntime *rt, dv_model_t *model);

  AraRuntime *mRt = nullptr;
  dv_model_t *mModel = nullptr;
  int mNumInputs = 0;
  int mNumOutputs = 0;
};

class AraRuntime {
public:
  AraRuntime() = default;
  ~AraRuntime();

  // dlopen the client lib, create a session on the given unix socket, and query
  // the default endpoint group. socketPath defaults to /var/run/proxy.sock.
  bool init(const std::string &socketPath = "/var/run/proxy.sock");

  // Load a compiled model.dvm. name is a free-form label. Returns nullptr on error.
  std::unique_ptr<AraModel> loadModel(const std::string &modelPath,
                                      const std::string &name);

  bool initialized() const { return mInitialized; }
  const char *lastError() const { return mLastError.c_str(); }

private:
  friend class AraModel;

  // dvapi function pointer typedefs (only the symbols we use).
  using fn_set_log_level = dv_status_code_t (*)(dv_client_log_level_t);
  using fn_stringify = const char *(*)(dv_status_code_t);
  using fn_session_unix = dv_status_code_t (*)(const char *, dv_session_t **);
  using fn_session_close = dv_status_code_t (*)(dv_session_t *);
  using fn_endpoint_get_list = dv_status_code_t (*)(dv_session_t *, dv_endpoint_t **, int *);
  using fn_model_load = dv_status_code_t (*)(dv_session_t *, dv_endpoint_t *,
                                             const char *, const char *,
                                             dv_model_priority_level_t, dv_model_t **);
  using fn_model_unload = dv_status_code_t (*)(dv_model_t *);
  using fn_infer_sync = dv_status_code_t (*)(dv_session_t *, dv_endpoint_t *,
                                             dv_model_t *, dv_blob_t *, dv_blob_t *,
                                             int, bool, dv_infer_request_t **);
  using fn_infer_free = dv_status_code_t (*)(dv_infer_request_t *);

  bool loadSymbols();
  bool check(const char *what, dv_status_code_t st);

  void *mLib = nullptr;
  dv_session_t *mSession = nullptr;
  dv_endpoint_t *mEndpoints = nullptr;
  int mEndpointCount = 0;
  bool mInitialized = false;
  std::string mLastError;

  fn_set_log_level pSetLogLevel = nullptr;
  fn_stringify pStringify = nullptr;
  fn_session_unix pSessionUnix = nullptr;
  fn_session_close pSessionClose = nullptr;
  fn_endpoint_get_list pEndpointGetList = nullptr;
  fn_model_load pModelLoad = nullptr;
  fn_model_unload pModelUnload = nullptr;
  fn_infer_sync pInferSync = nullptr;
  fn_infer_free pInferFree = nullptr;
};
