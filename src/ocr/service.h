#pragma once
#include "ocr/protocol.h"
#include "capture/frame.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
#include <filesystem>
#define LUMASHOT_OCR_SERVICE_PREWARM 1
namespace lumashot::ocr {
struct Completion {uint64_t id{},version{};Text text;std::wstring error;};
class Service {
public:
    Service(HWND notify,UINT message);
    ~Service();
    void Submit(uint64_t id,uint64_t version,std::shared_ptr<const Frame> image);
    void SetModelDirectory(const std::filesystem::path& directory);
    void Cancel(uint64_t id);
    // Starts the worker and loads the models ahead of a likely request. It
    // still exits after 30 seconds without work.
    void Prewarm();
    std::vector<Completion> Take();
private:
    struct Job {uint64_t id{},version{};std::shared_ptr<const Frame> image;std::shared_ptr<std::atomic_bool> canceled;std::filesystem::path model_directory;};
    void Run();void StartChild(const std::filesystem::path& directory);void StopChild();
    void Read(void* buffer,size_t size,const Job& job);
    HWND notify_;UINT message_;
    std::mutex mutex_;std::condition_variable cv_;std::deque<Job> jobs_;
    std::vector<Completion> completed_;std::optional<Job> active_;
    std::atomic_bool stopping_{};bool prewarm_{};
    Handle process_,input_,output_,job_object_;
    std::filesystem::path model_directory_,child_model_directory_;
    std::thread thread_;
};
}