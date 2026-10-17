#pragma once

#include <QObject>
#include <atomic>
#include <functional>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

class QThread;

namespace obs_seeker {
class PresentationEscape final : public QObject {
public:
    using Completion = std::function<void(bool)>;
    PresentationEscape(QObject *parent, std::function<void(Completion)> request);
    ~PresentationEscape() override;
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void start();
    void stop();
private:
    void captured(std::function<void()> deliver);
    std::function<void(Completion)> request_;
    std::atomic_bool enabled_{false};
    std::atomic_bool pending_{false};
#ifdef Q_OS_WIN
    QThread *thread_ = nullptr;
#elif defined(Q_OS_MACOS)
    CFMachPortRef tap_ = nullptr;
    CFRunLoopSourceRef source_ = nullptr;
    bool held_ = false;
#endif
};
}
