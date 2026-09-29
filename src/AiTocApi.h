/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

class EngineBase;

// Connection settings for AI TOC recognition (an OpenAI-compatible vision
// model API). Strings are borrowed; the caller owns them.
struct AiTocApiConfig {
    const char* baseUrl = nullptr;
    const char* key = nullptr;
    const char* model = nullptr;
    int concurrency = 4;
};

// Progress stages reported by AiTocApiProgressFn during recognition.
enum class AiTocApiStage {
    Render = 0,  // rendering TOC page images
    Extract = 1, // stage A: per-page title/page extraction
    Levels = 2,  // stage B: per-page level determination
    Offset = 3,  // stage C: AI page-offset sampling
};

typedef bool (*AiTocApiCancelFn)(void* ctx);
// Called from worker threads; marshal to the UI thread in the callback.
typedef void (*AiTocApiProgressFn)(void* ctx, AiTocApiStage stage, int done, int total);

bool AiTocApiIsConfigured(const AiTocApiConfig& cfg);

// Runs the autoContents two-stage printed-TOC recognition (port of the
// qwen_vl_extract + determine_toc_levels pipeline) plus on-demand AI page
// offset sampling. Blocking; call from a worker thread. On success
// *itemsJsonOut is a newly allocated {"items":[{title,page,level,pdf_page?}]}
// JSON string; *arabicOffsetOut / *romanOffsetOut are -1 when not computed.
bool AiTocApiRunRecognition(EngineBase* engine, const Vec<int>& pages, const AiTocApiConfig& cfg,
                            AiTocApiCancelFn canceled, void* cancelCtx, AiTocApiProgressFn progress, void* progressCtx,
                            char** itemsJsonOut, int* arabicOffsetOut, int* romanOffsetOut, char** errorOut);

// Sliding-window AI detection of the printed-TOC page range (port of the
// autoContents pdf_metadata_extractor detect flow). pagesOut receives every
// page of the detected continuous range. Blocking; call from a worker thread.
bool AiTocApiDetectTocPages(EngineBase* engine, const AiTocApiConfig& cfg, AiTocApiCancelFn canceled, void* cancelCtx,
                            AiTocApiProgressFn progress, void* progressCtx, Vec<int>& pagesOut, char** errorOut);

// Sends a minimal chat request to verify the endpoint/key/model. Blocking.
bool AiTocApiTestConnection(const char* baseUrl, const char* key, const char* model, char** msgOut);

// Queries the provider's OpenAI-compatible GET /models endpoint. Some
// providers do not expose it; callers should keep manual model entry usable.
bool AiTocApiFetchModels(const char* baseUrl, const char* key, Vec<char*>& modelsOut, char** errorOut);

// Reads base_url / api_key / model from an autoContents static/llm_config.json
// (multi-config + active_id format) or a flat {"base_url","api_key","model"}
// file. Outputs are newly allocated ("$ENV_VAR$" references are resolved).
bool AiTocApiLoadConfigFile(const char* path, char** baseUrlOut, char** keyOut, char** modelOut);
