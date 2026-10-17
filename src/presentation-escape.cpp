#include "presentation-escape.hpp"
#include <QPointer>
#include <utility>

namespace obs_seeker {
PresentationEscape::PresentationEscape(QObject *parent, std::function<void(Completion)> request)
    : QObject(parent), request_(std::move(request)) {}

PresentationEscape::~PresentationEscape() { stop(); }

void PresentationEscape::captured(std::function<void()> deliver)
{
    if (pending_.exchange(true))
        return;
    QMetaObject::invokeMethod(this, [this, deliver = std::move(deliver)]() mutable {
        request_([guard = QPointer<PresentationEscape>(this), deliver = std::move(deliver)](bool send) {
            if (!guard)
                return;
            if (send)
                deliver();
            guard->pending_ = false;
        });
    }, Qt::QueuedConnection);
}

}
