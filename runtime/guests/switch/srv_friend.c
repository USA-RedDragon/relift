/**
 * @file srv_friend.c
 * @brief The friend service friend:u (nn::friends::detail::ipc::IServiceCreator)
 * of a console that is offline and has no network service account. Command
 * ids and interfaces follow switchbrew.org (Friend services); the commands
 * a game calls that this does not answer stop with relift's unknown command
 * report.
 */
#include <string.h>

#include "hos.h"
#include "services.h"

/**
 * @brief IFriendService 10120 CheckFriendListAvailability(Uid): a bool.
 * Inferred: true. Ryujinx and Eglantine both answer true; the friend list is
 * the console's local cache, which exists (empty) without a network account.
 */
static rl_result check_friend_list(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 1);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    *o = 1;
    return KR_SUCCESS;
}

/**
 * @brief IFriendService 10700 GetPlayHistoryRegistrationKey(bool, Uid) into
 * a 0x40-byte auto-select buffer (SwIPC). Inferred: Ryujinx's layout, u16
 * type 0x101, u8 key index, u8 0, u8 the bool, 11 reserved bytes, a 16-byte
 * UUID, a 32-byte HMAC. The console draws a UUID once per user and keeps it
 * in its save data; relift uses the user's Uid as that UUID so it stays
 * stable. Key index 0 and a zero HMAC: the HMAC key is the console's and
 * only servers check it (Ryujinx's reasoning). Eglantine answers success
 * without writing the buffer, which leaves the game's key unset; Ryujinx's
 * pick follows the service's own layout.
 */
static rl_result play_history_key(rl_ipc_req *r)
{
    if (r->in_size < 24)
        return KR_INVALID_SIZE;
    uint8_t key[0x40] = { 0x01, 0x01 };
    key[4] = r->in[0] & 1;
    memcpy(key + 0x10, r->in + 8, 16);
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    if (out.size < sizeof key)
        return KR_INVALID_SIZE;
    return rl_ipc_buf_write(&out, key, sizeof key) ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

static const rl_ipc_cmd friend_cmds[] = {
    { 10120, "CheckFriendListAvailability", check_friend_list },
    { 10700, "GetPlayHistoryRegistrationKey", play_history_key },
};

static const rl_ipc_class friend_class = { "nn::friends::detail::ipc::IFriendService", friend_cmds,
                                           sizeof friend_cmds / sizeof friend_cmds[0], 0, NULL };

/** @brief IDaemonSuspendSessionService: switchbrew lists no commands. */
static const rl_ipc_class suspend_class = { "nn::friends::detail::ipc::IDaemonSuspendSessionService", NULL, 0, 0,
                                            NULL };

static rl_result push(rl_ipc_req *r, const rl_ipc_class *cls)
{
    rl_ipc_object *o = rl_srv_object(cls, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

/** @brief IServiceCreator 0: no input; an IFriendService. */
static rl_result create_friend_service(rl_ipc_req *r) { return push(r, &friend_class); }

/** @brief IServiceCreator 2: no input; an IDaemonSuspendSessionService. */
static rl_result create_suspend_service(rl_ipc_req *r) { return push(r, &suspend_class); }

static const rl_ipc_cmd creator_cmds[] = {
    { 0, "CreateFriendService", create_friend_service },
    { 2, "CreateDaemonSuspendSessionService", create_suspend_service },
};

static const rl_ipc_class creator_class = { "friend:u", creator_cmds, sizeof creator_cmds / sizeof creator_cmds[0], 0,
                                            NULL };

static rl_ipc_object *creator_factory(void) { return rl_srv_object(&creator_class, NULL); }

void rl_srv_install_friend(void) { rl_ipc_register_service("friend:u", creator_factory); }
