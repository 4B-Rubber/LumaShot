#pragma once
#include "ocr/text.h"
#include "capture/frame.h"
#include <filesystem>
#include <memory>
namespace lumashot::ocr {
struct Timings { double preprocess_ms{},detection_ms{},recognition_ms{},decode_ms{}; };
// threads 0 picks DefaultThreads().
struct EngineOptions { int threads{0}; };
#define LUMASHOT_OCR_ENGINE_OPTIONS 1
int DefaultThreads();
class Engine {
public:
    explicit Engine(const std::filesystem::path& assets,EngineOptions options={});
    ~Engine();
    Text Recognize(const Frame& frame,Timings* timings=nullptr);
    // Runs both models once on blank input so the first real request does not
    // pay for lazy runtime initialization and arena growth.
    void Warmup();
    int Threads() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}