/*
 * Small, local wrapper around obs-websocket's public Vendor API.
 *
 * This deliberately uses obs-websocket's process-handler API instead of
 * linking against its implementation, so OBS Replays remains a normal OBS
 * module and inherits OBS's existing websocket authentication and transport.
 */
#pragma once

#include <obs.h>

#include <assert.h>
#include <string.h>

typedef void *obs_websocket_vendor;
typedef void (*obs_websocket_request_callback_function)(obs_data_t *, obs_data_t *, void *);

struct obs_replays_websocket_request_callback {
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

static proc_handler_t *obs_replays_websocket_proc_handler = NULL;

static inline bool obs_replays_websocket_ensure_proc_handler(void)
{
	if (obs_replays_websocket_proc_handler)
		return true;

	proc_handler_t *global_handler = obs_get_proc_handler();
	assert(global_handler != NULL);
	calldata_t calldata = {0};
	if (!proc_handler_call(global_handler, "obs_websocket_api_get_ph", &calldata)) {
		blog(LOG_DEBUG, "OBS Replays: obs-websocket Vendor API is unavailable.");
		calldata_free(&calldata);
		return false;
	}
	obs_replays_websocket_proc_handler = static_cast<proc_handler_t *>(calldata_ptr(&calldata, "ph"));
	calldata_free(&calldata);
	return obs_replays_websocket_proc_handler != NULL;
}

static inline bool obs_replays_websocket_run_vendor_proc(obs_websocket_vendor vendor, const char *procedure,
							 calldata_t *calldata)
{
	if (!obs_replays_websocket_ensure_proc_handler() || !vendor || !procedure || !*procedure || !calldata)
		return false;
	calldata_set_ptr(calldata, "vendor", vendor);
	proc_handler_call(obs_replays_websocket_proc_handler, procedure, calldata);
	return calldata_bool(calldata, "success");
}

static inline obs_websocket_vendor obs_replays_websocket_register_vendor(const char *name)
{
	if (!obs_replays_websocket_ensure_proc_handler())
		return NULL;
	calldata_t calldata = {0};
	calldata_set_string(&calldata, "name", name);
	proc_handler_call(obs_replays_websocket_proc_handler, "vendor_register", &calldata);
	obs_websocket_vendor vendor = calldata_ptr(&calldata, "vendor");
	calldata_free(&calldata);
	return vendor;
}

static inline bool obs_replays_websocket_register_request(obs_websocket_vendor vendor, const char *type,
							  obs_websocket_request_callback_function callback,
							  void *priv_data)
{
	struct obs_replays_websocket_request_callback request_callback = {callback, priv_data};
	calldata_t calldata = {0};
	calldata_set_string(&calldata, "type", type);
	calldata_set_ptr(&calldata, "callback", &request_callback);
	const bool success = obs_replays_websocket_run_vendor_proc(vendor, "vendor_request_register", &calldata);
	calldata_free(&calldata);
	return success;
}

static inline bool obs_replays_websocket_emit_event(obs_websocket_vendor vendor, const char *type,
						    obs_data_t *event_data)
{
	calldata_t calldata = {0};
	calldata_set_string(&calldata, "type", type);
	calldata_set_ptr(&calldata, "data", event_data);
	const bool success = obs_replays_websocket_run_vendor_proc(vendor, "vendor_event_emit", &calldata);
	calldata_free(&calldata);
	return success;
}
