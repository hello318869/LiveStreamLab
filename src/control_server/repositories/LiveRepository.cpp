#include "LiveRepository.hpp"
#include <stdexcept>

namespace lsl::server {
namespace {
constexpr const char* sessionSelect =
    "SELECT l.*,r.owner_id,r.current_session_id,"
    "(l.prepare_deadline<=UTC_TIMESTAMP(6)) AS prepare_expired,"
    "(l.reconnect_deadline<=UTC_TIMESTAMP(6)) AS reconnect_expired "
    "FROM live_sessions l JOIN rooms r ON r.id=l.room_id ";

constexpr const char* roomSelect =
    "SELECT r.id,r.owner_id,u.nickname,r.title,r.category,r.current_session_id,"
    "COALESCE(l.status,'OFFLINE') AS status,l.started_at,r.updated_at "
    "FROM rooms r JOIN users u ON u.id=r.owner_id "
    "LEFT JOIN live_sessions l ON l.id=r.current_session_id AND l.room_id=r.id ";

} // namespace

const std::string& LiveRepository::sql(Operation operation) {
    switch (operation) {
    case Operation::LockOwnedRoom: {
        static const std::string statement = "SELECT id,current_session_id FROM rooms WHERE owner_id=? FOR UPDATE";
        return statement;
    }
    case Operation::ExistingSession: {
        static const std::string statement = "SELECT id,room_id,client_request_id,status FROM live_sessions WHERE room_id=? "
                    "AND (client_request_id=? OR status IN ('PREPARING','LIVE','RECONNECTING','STOPPING')) "
                    "ORDER BY (client_request_id=?) DESC LIMIT 1 FOR UPDATE";
        return statement;
    }
    case Operation::InsertSession: {
        static const std::string statement = "INSERT INTO live_sessions(id,room_id,client_request_id,stream_name,publish_token_hash,status,"
                            "allow_publish,created_at,prepare_deadline,publish_connected) "
                            "VALUES(?,?,?,?,UNHEX(?),'PREPARING',1,UTC_TIMESTAMP(6),TIMESTAMPADD(SECOND,?,UTC_TIMESTAMP(6)),0)";
        return statement;
    }
    case Operation::AttachSession: {
        static const std::string statement = "UPDATE rooms SET current_session_id=?,updated_at=UTC_TIMESTAMP(6) WHERE id=?";
        return statement;
    }
    case Operation::RawSession: {
        static const std::string statement = "SELECT * FROM live_sessions WHERE id=?";
        return statement;
    }
    case Operation::Session: {
        static const std::string statement = std::string(sessionSelect) + "WHERE l.id=?";
        return statement;
    }
    case Operation::StopSession: {
        static const std::string statement = "UPDATE live_sessions SET status='STOPPING',allow_publish=0,publish_token_hash=NULL,"
                "reconnect_deadline=NULL,end_reason='OWNER_STOPPED',revision=revision+1 "
                "WHERE id=? AND status IN ('PREPARING','LIVE','RECONNECTING')";
        return statement;
    }
    case Operation::Lobby: {
        static const std::string statement = std::string(roomSelect) + "WHERE l.status='LIVE' ORDER BY l.started_at DESC,r.id DESC LIMIT ? OFFSET ?";
        return statement;
    }
    case Operation::Room: {
        static const std::string statement = std::string(roomSelect) + "WHERE r.id=?";
        return statement;
    }
    case Operation::Playback: {
        static const std::string statement = "SELECT r.id,l.id AS live_session_id,l.status,l.stream_name FROM rooms r "
          "LEFT JOIN live_sessions l ON l.id=r.current_session_id AND l.room_id=r.id WHERE r.id=?";
        return statement;
    }
    case Operation::Stream: {
        static const std::string statement = std::string(sessionSelect) + "WHERE l.stream_name=?";
        return statement;
    }
    case Operation::Unpublish: {
        static const std::string statement = "UPDATE live_sessions SET publish_connected=0,"
                    "reconnect_deadline=CASE WHEN status='LIVE' THEN TIMESTAMPADD(SECOND,?,UTC_TIMESTAMP(6)) ELSE reconnect_deadline END,"
                    "status=CASE WHEN status='LIVE' THEN 'RECONNECTING' ELSE status END,"
                    "revision=revision+1 WHERE id=? AND revision=? AND publish_generation=? AND srs_server_id=? "
                    "AND srs_service_id=? AND srs_client_id=?";
        return statement;
    }
    case Operation::AdmitPublish: {
        static const std::string statement = "UPDATE live_sessions SET srs_server_id=?,srs_service_id=?,srs_stream_id=NULLIF(?,''),srs_client_id=?,"
                "publish_connected=1,publish_accepted_at=UTC_TIMESTAMP(6),publish_generation=publish_generation+1,revision=revision+1,"
                "srs_recv_bytes=NULL,srs_audio_frames=NULL,srs_video_frames=NULL,last_confirmed_at=UTC_TIMESTAMP(6),last_media_progress_at=NULL "
                "WHERE id=? AND revision=? AND publish_generation=? AND allow_publish=1 AND publish_connected=0 "
                "AND status IN ('PREPARING','RECONNECTING') "
                "AND (status<>'PREPARING' OR prepare_deadline>UTC_TIMESTAMP(6)) "
                "AND (status<>'RECONNECTING' OR reconnect_deadline>UTC_TIMESTAMP(6))";
        return statement;
    }
    case Operation::Nonterminal: {
        static const std::string statement = std::string(sessionSelect) + "WHERE l.status IN ('PREPARING','LIVE','RECONNECTING','STOPPING')";
        return statement;
    }
    case Operation::LockRoom: {
        static const std::string statement = "SELECT id FROM rooms WHERE id=? FOR UPDATE";
        return statement;
    }
    case Operation::FinishSession: {
        static const std::string statement = "UPDATE live_sessions SET status=?,allow_publish=0,publish_token_hash=NULL,publish_connected=0,"
                    "ended_at=UTC_TIMESTAMP(6),reconnect_deadline=NULL,end_reason=?,revision=revision+1 "
                    "WHERE id=? AND revision=? AND publish_generation=? "
                    "AND status IN ('PREPARING','LIVE','RECONNECTING','STOPPING')";
        return statement;
    }
    case Operation::ClearRoom: {
        static const std::string statement = "UPDATE rooms SET current_session_id=NULL,updated_at=UTC_TIMESTAMP(6) WHERE id=? AND current_session_id=?";
        return statement;
    }
    case Operation::ClearPreparingConnection: {
        static const std::string statement = "UPDATE live_sessions SET publish_connected=0,revision=revision+1,last_confirmed_at=UTC_TIMESTAMP(6),"
              "srs_audio_frames=NULL,srs_video_frames=NULL,srs_recv_bytes=NULL,last_media_progress_at=NULL "
              "WHERE id=? AND revision=? AND publish_generation=? AND status='PREPARING'";
        return statement;
    }
    case Operation::Recover: {
        static const std::string statement = "UPDATE live_sessions SET reconnect_deadline="
        "CASE WHEN status='LIVE' THEN TIMESTAMPADD(SECOND,?,UTC_TIMESTAMP(6)) ELSE reconnect_deadline END,"
        "status='RECONNECTING',publish_connected=?,last_confirmed_at=UTC_TIMESTAMP(6),revision=revision+1 "
        "WHERE id=? AND revision=? AND publish_generation=? AND status IN ('LIVE','RECONNECTING')";
        return statement;
    }
    case Operation::TimeoutStop: {
        static const std::string statement = "UPDATE live_sessions SET status='STOPPING',allow_publish=0,publish_token_hash=NULL,"
                "reconnect_deadline=NULL,end_reason=?,revision=revision+1 WHERE id=? AND revision=? AND publish_generation=?";
        return statement;
    }
    case Operation::Progress: {
        static const std::string statement = "UPDATE live_sessions SET srs_stream_id=?,publish_connected=1,last_confirmed_at=UTC_TIMESTAMP(6),"
            "srs_recv_bytes=?,srs_audio_frames=?,srs_video_frames=?,"
            "last_media_progress_at=CASE WHEN ? THEN UTC_TIMESTAMP(6) ELSE last_media_progress_at END,"
            "started_at=CASE WHEN ? THEN COALESCE(started_at,UTC_TIMESTAMP(6)) ELSE started_at END,"
            "reconnect_deadline=CASE WHEN ? THEN NULL ELSE reconnect_deadline END,"
            "status=CASE WHEN ? THEN 'LIVE' ELSE status END,revision=revision+1 "
            "WHERE id=? AND revision=? AND publish_generation=? AND srs_server_id=? AND srs_service_id=? AND srs_client_id=? "
            "AND allow_publish=1 AND status IN ('PREPARING','LIVE','RECONNECTING')";
        return statement;
    }
    }
    throw std::logic_error("Invalid live repository operation");
}
} // namespace lsl::server

