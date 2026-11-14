/**
 * @file srv_acc.c
 * @brief The account service for applications, acc:u0, with one local user
 * that is always open: the preselected user of the applet manager.
 *
 * Command ids, argument layouts and results follow Ryujinx (MIT)
 * Services/Account: IAccountServiceForApplication, IProfile and
 * IManagerForApplication, with results in module 124.
 */
#include "services.h"
#include "srv_am_priv.h"

#include <string.h>

#define ACC_RESULT(desc) RL_MAKE_RESULT(124, desc)
#define ACC_NULL_ARGUMENT ACC_RESULT(20)
#define ACC_INVALID_BUFFER ACC_RESULT(32)
#define ACC_USER_NOT_FOUND ACC_RESULT(100)

#define ACC_POINTER_BUFFER_SIZE 0x1000
#define ACC_USER_DATA_SIZE 0x80u
#define ACC_NICKNAME_SIZE 0x20u
#define ACC_NICKNAME "relift"

typedef struct acc_uid {
    uint64_t v[2];
} acc_uid;

typedef struct acc_profile_base {
    acc_uid uid;
    uint64_t last_edit;
    char nickname[ACC_NICKNAME_SIZE];
} acc_profile_base;

static acc_uid the_user(void)
{
    acc_uid u;
    am_preselected_user(&u.v[0], &u.v[1]);
    return u;
}

static bool is_the_user(const acc_uid *u)
{
    acc_uid t = the_user();
    return u->v[0] == t.v[0] && u->v[1] == t.v[1];
}

static rl_result read_uid(rl_ipc_req *r, acc_uid *u)
{
    if (!rl_srv_in(r, u, sizeof *u))
        return SF_RESULT_INVALID_HEADER;
    if (!u->v[0] && !u->v[1])
        return ACC_NULL_ARGUMENT;
    return KR_SUCCESS;
}

static rl_result write_user_list(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    acc_uid list[8];
    uint64_t n = out.size < sizeof list ? out.size : sizeof list;
    if (!out.size)
        return ACC_INVALID_BUFFER;
    memset(list, 0, sizeof list);
    list[0] = the_user();
    if (!rl_ipc_buf_write(&out, list, n))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static rl_result get_user_count(rl_ipc_req *r)
{
    *(int32_t *)rl_ipc_out(r, 4) = 1;
    return KR_SUCCESS;
}

static rl_result get_user_existence(rl_ipc_req *r)
{
    acc_uid u;
    rl_result rc = read_uid(r, &u);
    if (rc)
        return rc;
    *(uint8_t *)rl_ipc_out(r, 1) = is_the_user(&u);
    return KR_SUCCESS;
}

static rl_result get_last_opened_user(rl_ipc_req *r)
{
    acc_uid u = the_user();
    memcpy(rl_ipc_out(r, sizeof u), &u, sizeof u);
    return KR_SUCCESS;
}

static void fill_base(acc_profile_base *b)
{
    memset(b, 0, sizeof *b);
    b->uid = the_user();
    memcpy(b->nickname, ACC_NICKNAME, sizeof ACC_NICKNAME - 1);
}

static rl_result profile_get_base(rl_ipc_req *r)
{
    acc_profile_base b;
    fill_base(&b);
    memcpy(rl_ipc_out(r, sizeof b), &b, sizeof b);
    return KR_SUCCESS;
}

static rl_result profile_get(rl_ipc_req *r)
{
    uint8_t data[ACC_USER_DATA_SIZE] = { 0 };
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint64_t n = out.size < sizeof data ? out.size : sizeof data;
    data[4] = 1;
    data[8] = 1;
    if (n && !rl_ipc_buf_write(&out, data, n))
        return KR_INVALID_MEM_STATE;
    return profile_get_base(r);
}

static rl_result profile_no_image(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd profile_cmds[] = {
    { 0, "Get", profile_get },
    { 1, "GetBase", profile_get_base },
    { 10, "GetImageSize", profile_no_image },
    { 11, "LoadImage", profile_no_image },
};

static const rl_ipc_class profile_class = { "nn::account::profile::IProfile", profile_cmds, AM_N(profile_cmds),
                                            ACC_POINTER_BUFFER_SIZE, NULL };

static rl_result get_profile(rl_ipc_req *r)
{
    acc_uid u;
    rl_result rc = read_uid(r, &u);
    if (rc)
        return rc;
    if (!is_the_user(&u))
        return ACC_USER_NOT_FOUND;
    return rl_ipc_push_object(r, rl_srv_object(&profile_class, NULL));
}

static rl_result out_false(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 0;
    return KR_SUCCESS;
}

static rl_result try_select_user(rl_ipc_req *r) { return get_last_opened_user(r); }

static rl_result manager_check_availability(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result manager_get_account_id(rl_ipc_req *r)
{
    acc_uid u = the_user();
    *(uint64_t *)rl_ipc_out(r, 8) = u.v[0] ^ u.v[1];
    return KR_SUCCESS;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "CheckAvailability", manager_check_availability },
    { 1, "GetAccountId", manager_get_account_id },
    RL_SRV_STUB(160, "StoreOpenContext"),
};

static const rl_ipc_class manager_class = { "nn::account::baas::IManagerForApplication", manager_cmds,
                                            AM_N(manager_cmds), ACC_POINTER_BUFFER_SIZE, NULL };

static rl_result get_manager(rl_ipc_req *r)
{
    acc_uid u;
    rl_result rc = read_uid(r, &u);
    if (rc)
        return rc;
    return rl_ipc_push_object(r, rl_srv_object(&manager_class, NULL));
}

static rl_result check_uid(rl_ipc_req *r)
{
    acc_uid u;
    return read_uid(r, &u);
}

static const rl_ipc_cmd service_cmds[] = {
    { 0, "GetUserCount", get_user_count },
    { 1, "GetUserExistence", get_user_existence },
    { 2, "ListAllUsers", write_user_list },
    { 3, "ListOpenUsers", write_user_list },
    { 4, "GetLastOpenedUser", get_last_opened_user },
    { 5, "GetProfile", get_profile },
    { 50, "IsUserRegistrationRequestPermitted", out_false },
    { 51, "TrySelectUserWithoutInteraction", try_select_user },
    { 60, "ListOpenContextStoredUsers", write_user_list },
    RL_SRV_STUB(100, "InitializeApplicationInfo"),
    { 101, "GetBaasAccountManagerForApplication", get_manager },
    { 110, "StoreSaveDataThumbnail", check_uid },
    { 111, "ClearSaveDataThumbnail", check_uid },
    { 130, "LoadOpenContext", get_manager },
    { 131, "ListOpenContextStoredUsers", write_user_list },
    RL_SRV_STUB(140, "InitializeApplicationInfo"),
    { 141, "ListQualifiedUsers", write_user_list },
    { 150, "IsUserAccountSwitchLocked", out_false },
    RL_SRV_STUB(160, "InitializeApplicationInfoV2"),
};

static const rl_ipc_class service_class = { "nn::account::IAccountServiceForApplication", service_cmds,
                                            AM_N(service_cmds), ACC_POINTER_BUFFER_SIZE, NULL };

static rl_ipc_object *service_factory(void) { return rl_srv_object(&service_class, NULL); }

void rl_srv_install_acc(void) { rl_ipc_register_service("acc:u0", service_factory); }
