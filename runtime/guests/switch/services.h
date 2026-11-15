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
/** @brief Registers acc:u0 with one local user, the applet manager's preselected user. */
void rl_srv_install_acc(void);
/** @brief Registers mm:u, the multimedia clock request service. */
void rl_srv_install_mm(void);
/** @brief Registers csrng, the random number service. */
void rl_srv_install_csrng(void);

/** @brief Registers every service of this file's group with sm:. Called by rl_hos_init. */
RL_RUNTIME_API void rl_srv_install_all(void);

#endif
