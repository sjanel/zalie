#include "zalie/ui/perf-tab.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <utility>

#include "zalie/load-plan.hpp"
#include "zalie/load-preview.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/variables.hpp"

namespace zalie::ui {

namespace {

constexpr std::uint32_t kNbPreviewRequests = 5;

}  // namespace

PlanCompiler::PlanCompiler(WakeFn wakeFn, void* pWakeUserData)
    : _wakeFn(wakeFn),
      _pWakeUserData(pWakeUserData),
      _thread([this](const std::stop_token& stopToken) { run(stopToken); }) {}

PlanCompiler::~PlanCompiler() {
  _thread.request_stop();
  _condition.notify_all();
}

void PlanCompiler::submit(Job job) {
  {
    std::scoped_lock lock(_mutex);
    auto it = std::ranges::find_if(_jobs, [&job](const Job& queued) { return queued.slot == job.slot; });
    if (it != _jobs.end()) {
      *it = std::move(job);
    } else {
      _jobs.push_back(std::move(job));
    }
  }
  _condition.notify_all();
}

bool PlanCompiler::waitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(_mutex);
  return _condition.wait_for(lock, timeout, [this] { return _jobs.empty() && !_busy; });
}

void PlanCompiler::run(const std::stop_token& stopToken) {
  while (true) {
    Job job;
    {
      std::unique_lock lock(_mutex);
      _busy = false;
      _condition.notify_all();
      if (!_condition.wait(lock, stopToken, [this] { return !_jobs.empty(); })) {
        return;
      }
      job = std::move(_jobs.front());
      _jobs.pop_front();
      _busy = true;
    }
    std::shared_ptr<const LoadPreview> preview;
    try {
      const VariableScope scope = MakeScope(job.context, job.context.environments.empty() ? -1 : 0);
      assert(job.request.load.has_value());
      const LoadSettings& settings = *job.request.load;
      preview = std::make_shared<const LoadPreview>(MakeLoadPreview(job.request, job.context, scope, settings,
                                                                    job.baseDirectory, _dataFiles, kNbPreviewRequests));
    } catch (const std::exception& ex) {
      // Out of memory, a path that cannot be converted...: the run cannot start, the tab tells why.
      auto failed = std::make_shared<LoadPreview>();
      failed->plan = std::make_shared<const LoadPlan>(ResolvedRequest{});
      failed->error = Concat({"Cannot prepare the run: ", ex.what()});
      preview = std::move(failed);
    }
    {
      std::scoped_lock lock(job.slot->mutex);
      if (job.revision > job.slot->revision) {
        job.slot->preview = std::move(preview);
        job.slot->revision = job.revision;
      }
    }
    if (_wakeFn != nullptr) {
      _wakeFn(_pWakeUserData);
    }
  }
}

}  // namespace zalie::ui
