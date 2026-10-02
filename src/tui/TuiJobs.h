#ifndef LONELYICE_TUIJOBS_H
#define LONELYICE_TUIJOBS_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "Lang.h"

namespace LonelyIce
{
    // One operation at a time. Workers return an application callback; only Poll runs it on the UI thread.
    class TuiJobs
    {
    public:
        using Completion = std::function<void()>;
        using Work = std::function<Completion(std::atomic<bool> const&)>;

        explicit TuiJobs(std::function<void()> wake, std::function<void(std::string)> error)
            : _wake(std::move(wake)), _error(std::move(error))
        {
        }
        ~TuiJobs()
        {
            Cancel();
            if (_thread.joinable())
                _thread.join();
        }
        TuiJobs(TuiJobs const&) = delete;
        TuiJobs& operator=(TuiJobs const&) = delete;

        bool Busy() const
        {
            return _busy;
        }
        void Cancel()
        {
            _cancel = true;
        }
        bool Start(Work work)
        {
            if (_busy)
                return false;
            if (_thread.joinable())
                _thread.join();
            _cancel = false;
            _busy = true;
            _thread = std::thread(
                [this, work = std::move(work)]
                {
                    Completion done;
                    try
                    {
                        done = work(_cancel);
                    }
                    catch (std::exception const& e)
                    {
                        done = [this, message = std::string(e.what())] { _error(message); };
                    }
                    catch (...)
                    {
                        done = [this] { _error(Tr("tui.worker_failure")); };
                    }
                    {
                        std::lock_guard guard(_mutex);
                        _done = std::move(done);
                        _finished = true;
                    }
                    _wake();
                });
            return true;
        }
        void Poll()
        {
            Completion done;
            {
                std::lock_guard guard(_mutex);
                if (!_finished)
                    return;
                done = std::move(_done);
                _finished = false;
            }
            if (_thread.joinable())
                _thread.join();
            _busy = false;
            if (done)
                done();
        }

    private:
        std::function<void()> _wake;
        std::function<void(std::string)> _error;
        std::atomic<bool> _cancel{false};
        std::thread _thread;
        std::mutex _mutex;
        Completion _done;
        bool _busy = false, _finished = false;
    };
} // namespace LonelyIce
#endif
