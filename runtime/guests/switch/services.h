/**
 * @file services.h
 * @brief The Horizon services the runtime implements (everything except
 * fsp-srv and the audio services), their shared helpers and installation.
 *
 * Command ids, result codes and structure layouts follow Ryujinx (MIT) and
 * switchbrew.org. A command that only returns success (or a fixed value)
 * because no program can tell the difference is listed with RL_SRV_STUB and
 * shows a trailing asterisk in its trace name.
 */
#ifndef RELIFT_SERVICES_H
#define RELIFT_SERVICES_H

#include "ipc.h"

/** @brief Defines a command table entry for a stub: it succeeds with no output. */
#define RL_SRV_STUB(id, name) { (id), name "*", rl_srv_stub_ok }

/** @brief Handler of stub commands: returns success with no output. */
rl_result rl_srv_stub_ok(rl_ipc_req *r);
/** @brief Creates an object of cls with state (a thin wrapper that keeps service code short). */
rl_ipc_object *rl_srv_object(const rl_ipc_class *cls, void *state);
/** @brief Reads one request structure; false when the client sent fewer than size bytes. */
bool rl_srv_in(const rl_ipc_req *r, void *dst, uint32_t size);
/** @brief Returns the client's process id for requests that carry one, else HOS_PID. */
uint64_t rl_srv_pid(const rl_ipc_req *r);

/** @brief Installs set, set:sys and the other settings services. */
void rl_srv_install_set(void);
/** @brief Installs appletOE, appletAE and the am objects. */
void rl_srv_install_am(void);
/** @brief Installs apm, apm:sys, apm:p and psm. */
void rl_srv_install_apm(void);
/** @brief Installs pctl, pctl:a, pctl:r and pctl:s, the parental control services. */
void rl_srv_install_pctl(void);
/** @brief Installs aoc:u, the add-on content manager. */
void rl_srv_install_aoc(void);
/** @brief Installs lm, the log manager. */
void rl_srv_install_lm(void);
/** @brief Installs hid, hid:sys, hid:dbg and irs. */
void rl_srv_install_hid(void);
/** @brief Installs time:u, time:a, time:s, time:su, time:r. */
void rl_srv_install_time(void);
/** @brief Installs vi:m, vi:s, vi:u, the binder driver and the buffer queue. */
void rl_srv_install_vi(void);
/** @brief Installs nvdrv, nvdrv:a, nvdrv:s, nvdrv:t. */
void rl_srv_install_nv(void);
/** @brief Registers fsp-srv with the SD card on $RELIFT_SDMC (default ./sdmc), creating it. */
void rl_srv_install_fs(void);
/** @brief Declares the settings of the Switch layer (and the core's); idempotent. */
void rl_switch_declare_settings(void);
/** @brief Declares the Switch ports and their bind.* settings; idempotent. Call before reading arguments so --relift-set and settings files can rebind keys. */
void rl_switch_declare_ports(void);
/** @brief Declares the on-screen controller port once it is wanted; call again after reading arguments, which can ask for it. */
void rl_switch_declare_touchpad_port(void);
/** @brief Sets the process's pointer authentication keys and cipher from the cpu.pac settings. */
void rl_switch_apply_pac_settings(void);
/** @brief Registers acc:u0 with one local user, the applet manager's preselected user. */
void rl_srv_install_acc(void);
/** @brief Registers mm:u, the multimedia clock request service. */
void rl_srv_install_mm(void);
/** @brief Registers csrng, the random number service. */
void rl_srv_install_csrng(void);
/** @brief Opens a session to a new INvDrv2User (Switch 2 nvdrv), for gsv's CreateNvDrv2UserSession*. */
rl_result rl_srv_nv2_open(uint32_t *handle);
/** @brief Opens a session to a new gsv UserSession (Switch 2), for am's GetGraphicsSupervisor. */
rl_result rl_gsv_open_user_session(uint32_t *handle);
/** @brief Opens a video decoder session (gsv 902, 905, 906, 1002): one moved session handle in r. */
rl_result rl_gsv_open_video_session(rl_ipc_req *r);
rl_result rl_gsv_native_memory_create(rl_ipc_req *r);
rl_result rl_gsv_native_memory_destroy(rl_ipc_req *r);
rl_result rl_gsv_native_memory_bind(rl_ipc_req *r);
rl_result rl_gsv_native_image_query(rl_ipc_req *r);
rl_result rl_gsv_native_image_unmap(rl_ipc_req *r);
/** @brief The guest memory of a decoder native image (address and size), marking it as read by the GPU; false when it is unknown or not bound to memory. */
bool rl_gsv_native_image_region(uint32_t handle, uint64_t *addr, uint64_t *size);
rl_result rl_gsv_decoder_image_free(rl_ipc_req *r);
/** @brief One decoded pixel as 8-bit RGBA, or BGRA when bgra (BT.709, limited range). */
void rl_gsv_yuv_to_rgba(uint8_t y, uint8_t u, uint8_t v, bool bgra, uint8_t out[4]);
rl_result rl_gsv_decoder_pool_size(rl_ipc_req *r);
rl_result rl_gsv_decoder_pool_init(rl_ipc_req *r);
rl_result rl_gsv_decoder_image_descriptor(rl_ipc_req *r);
/** @brief Signals e on every vsync while on (a display session slot bound to vsync); starts the vsync source. e must outlive the listening: remove it before it is freed. */
void rl_vi_vsync_listen(hos_revent *e, bool on);
/** @brief Opens a syncpt session (gsv 202): one moved session handle in r. */
rl_result rl_gsv_open_syncpt_session(rl_ipc_req *r);
/** @brief Called with the nv lock held after a sync point advanced: fires the slot events waiting on it. */
void rl_gsv_syncpt_changed(void);
/** @brief A new counter of the supervisor's sync points: its id (the SyncCounterId is its bitwise NOT), starting at 0. */
uint32_t rl_gsv_counter_new(void);
/** @brief The value of counter id. */
uint64_t rl_gsv_counter_get(uint32_t id);
/** @brief Raises counter id to at least value and signals the waits it reaches. */
void rl_gsv_counter_signal(uint32_t id, uint64_t value);
/** @brief Registers ectx:aw, the error context writer, and notif:a, the alarm notifications. */
void rl_srv_install_ectx(void);
/** @brief Registers bsd:u and bsd:s, the socket services, without a network. */
void rl_srv_install_bsd(void);
/** @brief Registers bcat:u with an empty delivery cache and, on the Switch 2, stpl:u without stream play. */
void rl_srv_install_bcat(void);
void rl_srv_install_chat(void);
void rl_srv_install_pl(void);
void rl_srv_install_auddmg(void);
void rl_srv_install_audsmx(void);
/** @brief Registers friend:u (srv_friend.c). */
void rl_srv_install_friend(void);
/** @brief Registers nifm:u (srv_nifm.c). */
void rl_srv_install_nifm(void);
/** @brief Installs nsd:u and nsd:a, the network settings services. */
void rl_srv_install_nsd(void);
/** @brief Installs prepo:u and the other play report services. */
void rl_srv_install_prepo(void);
/** @brief Installs nfp:user, the NFC service. */
void rl_srv_install_nfp(void);
/** @brief Installs ssl and ssl:s, the TLS services. */
void rl_srv_install_ssl(void);

/** @brief Registers every service of this file's group with sm:. Called by rl_hos_init. */
RL_RUNTIME_API void rl_srv_install_all(void);

#endif
