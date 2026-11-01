#pragma once
#include "control_server/common/Support.hpp"

namespace lsl::server {
// All SQL stays in the repository; LiveService owns state-transition decisions,
// transaction lifetime and SRS coordination. Values always use bound arguments.
class LiveRepository {
public:
    enum class Operation {
        LockOwnedRoom,
        ExistingSession,
        InsertSession,
        AttachSession,
        RawSession,
        Session,
        StopSession,
        Lobby,
        Room,
        Playback,
        Stream,
        Unpublish,
        AdmitPublish,
        Nonterminal,
        LockRoom,
        FinishSession,
        ClearRoom,
        ClearPreparingConnection,
        Recover,
        TimeoutStop,
        Progress
    };
    template <class Client, class Callback, class... Args>
    static void exec(const std::shared_ptr<Client>& client, Operation operation,
                     Callback callback, Fail fail, Args&&... args) {
        query(client, sql(operation), std::move(callback), std::move(fail),
              std::forward<Args>(args)...);
    }
private:
    static const std::string& sql(Operation operation);
};
} // namespace lsl::server
