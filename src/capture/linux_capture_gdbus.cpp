#include "glyphrelay/linux_capture.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace glyphrelay {
namespace {

constexpr const char *kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char *kRequestInterface = "org.freedesktop.portal.Request";
constexpr const char *kSessionInterface = "org.freedesktop.portal.Session";
constexpr std::uint32_t kSourceTypeWindow = 2U;
constexpr std::uint32_t kCursorHidden = 1U;
constexpr std::uint32_t kCursorEmbedded = 2U;
constexpr std::uint32_t kCursorMetadata = 4U;

struct VariantUnref {
  void operator()(GVariant *value) const {
    if (value != nullptr) {
      g_variant_unref(value);
    }
  }
};
using VariantPtr = std::unique_ptr<GVariant, VariantUnref>;

struct ObjectUnref {
  void operator()(gpointer value) const {
    if (value != nullptr) {
      g_object_unref(value);
    }
  }
};
using FdListPtr = std::unique_ptr<GUnixFDList, ObjectUnref>;

std::string request_token() {
  std::unique_ptr<gchar, decltype(&g_free)> uuid(g_uuid_string_random(), &g_free);
  std::string token = "glyphrelay_";
  for (const char value : std::string_view(uuid.get())) {
    if (value != '-') {
      token.push_back(value);
    }
  }
  return token;
}

GVariant *finish_options(GVariantBuilder &builder) {
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

GVariant *create_options(const std::string &handle_token, const std::string &session_handle_token) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token",
                        g_variant_new_string(handle_token.c_str()));
  g_variant_builder_add(&builder, "{sv}", "session_handle_token",
                        g_variant_new_string(session_handle_token.c_str()));
  return finish_options(builder);
}

GVariant *select_options(const std::string &handle_token, CursorMode cursor_mode) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token",
                        g_variant_new_string(handle_token.c_str()));
  g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(kSourceTypeWindow));
  g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&builder, "{sv}", "cursor_mode",
                        g_variant_new_uint32(cursor_mode == CursorMode::metadata ? kCursorMetadata
                                             : cursor_mode == CursorMode::embedded
                                                 ? kCursorEmbedded
                                                 : kCursorHidden));
  g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(0U));
  return finish_options(builder);
}

GVariant *handle_options(const std::string &handle_token) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token",
                        g_variant_new_string(handle_token.c_str()));
  return finish_options(builder);
}

GVariant *empty_options() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  return finish_options(builder);
}

CursorMode select_cursor_mode(std::uint32_t available) {
  if ((available & kCursorMetadata) != 0U) {
    return CursorMode::metadata;
  }
  if ((available & kCursorEmbedded) != 0U) {
    return CursorMode::embedded;
  }
  return CursorMode::hidden;
}

struct AwaitedResponse {
  std::string expected_path;
  VariantPtr parameters;
};

void response_signal(GDBusConnection *, const gchar *, const gchar *object_path, const gchar *,
                     const gchar *, GVariant *parameters, gpointer user_data) {
  auto &response = *static_cast<AwaitedResponse *>(user_data);
  if (response.parameters == nullptr && !response.expected_path.empty() &&
      response.expected_path == object_path) {
    response.parameters.reset(g_variant_ref(parameters));
  }
}

struct RequestResult {
  bool passed = false;
  bool cancelled = false;
  std::string reason;
  std::string request_handle;
  VariantPtr results;
};

} // namespace

PortalWindowGrant::PortalWindowGrant(int pipewire_remote_fd, std::uint32_t pipewire_node_id,
                                     CursorMode cursor_mode)
    : pipewire_remote_fd_(pipewire_remote_fd), pipewire_node_id_(pipewire_node_id),
      cursor_mode_(cursor_mode) {}

PortalWindowGrant::~PortalWindowGrant() { reset(); }

PortalWindowGrant::PortalWindowGrant(PortalWindowGrant &&other) noexcept
    : pipewire_remote_fd_(std::exchange(other.pipewire_remote_fd_, -1)),
      pipewire_node_id_(std::exchange(other.pipewire_node_id_, 0U)),
      cursor_mode_(other.cursor_mode_) {}

PortalWindowGrant &PortalWindowGrant::operator=(PortalWindowGrant &&other) noexcept {
  if (this != &other) {
    reset();
    pipewire_remote_fd_ = std::exchange(other.pipewire_remote_fd_, -1);
    pipewire_node_id_ = std::exchange(other.pipewire_node_id_, 0U);
    cursor_mode_ = other.cursor_mode_;
  }
  return *this;
}

PortalWindowGrant::operator bool() const {
  return pipewire_remote_fd_ >= 0 && pipewire_node_id_ != 0U;
}

std::uint32_t PortalWindowGrant::pipewire_node_id() const { return pipewire_node_id_; }
CursorMode PortalWindowGrant::cursor_mode() const { return cursor_mode_; }

int PortalWindowGrant::release_pipewire_remote_fd() {
  pipewire_node_id_ = 0U;
  return std::exchange(pipewire_remote_fd_, -1);
}

void PortalWindowGrant::reset() {
  if (pipewire_remote_fd_ >= 0) {
    ::close(pipewire_remote_fd_);
  }
  pipewire_remote_fd_ = -1;
  pipewire_node_id_ = 0U;
}

struct LinuxPortalClient::Implementation {
  GMainContext *context = g_main_context_new();
  GDBusConnection *connection = nullptr;
  gulong connection_closed_handler = 0UL;
  guint session_closed_subscription = 0U;
  std::string session_handle;
  PortalSelectionStateMachine lifecycle;
  std::optional<CaptureState> terminal;

  ~Implementation() {
    close_session();
    if (connection != nullptr) {
      if (connection_closed_handler != 0UL) {
        g_signal_handler_disconnect(connection, connection_closed_handler);
      }
      g_object_unref(connection);
    }
    g_main_context_unref(context);
  }

  static void connection_closed(GDBusConnection *, gboolean, GError *, gpointer user_data) {
    auto &self = *static_cast<Implementation *>(user_data);
    self.terminal = CaptureState::disconnected;
  }

  static void session_closed(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                             const gchar *, GVariant *, gpointer user_data) {
    auto &self = *static_cast<Implementation *>(user_data);
    self.terminal = CaptureState::revoked;
  }

  bool connect_bus(std::uint32_t timeout_ms, std::string &reason) {
    if (connection != nullptr) {
      return true;
    }
    GError *error = nullptr;
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
      if (error != nullptr) {
        g_error_free(error);
      }
      reason = "portal_session_bus_unavailable";
      return false;
    }
    connection_closed_handler =
        g_signal_connect(connection, "closed", G_CALLBACK(Implementation::connection_closed), this);
    if (timeout_ms == 0U) {
      reason = "portal_timeout_invalid";
      return false;
    }
    return true;
  }

  std::optional<std::uint32_t> property(std::string_view name, std::uint32_t timeout_ms) {
    GError *error = nullptr;
    VariantPtr result(g_dbus_connection_call_sync(
        connection, kPortalName, kPortalPath, kPropertiesInterface, "Get",
        g_variant_new("(ss)", kScreenCastInterface, std::string(name).c_str()),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, static_cast<gint>(timeout_ms), nullptr,
        &error));
    if (result == nullptr) {
      if (error != nullptr) {
        g_error_free(error);
      }
      return std::nullopt;
    }
    GVariant *boxed = nullptr;
    g_variant_get(result.get(), "(@v)", &boxed);
    VariantPtr boxed_value(boxed);
    VariantPtr value(g_variant_get_variant(boxed_value.get()));
    if (!g_variant_is_of_type(value.get(), G_VARIANT_TYPE_UINT32)) {
      return std::nullopt;
    }
    return g_variant_get_uint32(value.get());
  }

  RequestResult call_request(const char *method, GVariant *parameters, std::uint32_t timeout_ms,
                             const std::function<bool(std::string_view)> &accept_handle) {
    AwaitedResponse awaited;
    const auto subscription = g_dbus_connection_signal_subscribe(
        connection, kPortalName, kRequestInterface, "Response", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, response_signal, &awaited, nullptr);
    GError *error = nullptr;
    VariantPtr request(g_dbus_connection_call_sync(connection, kPortalName, kPortalPath,
                                                   kScreenCastInterface, method, parameters,
                                                   G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                                   static_cast<gint>(timeout_ms), nullptr, &error));
    if (request == nullptr) {
      g_dbus_connection_signal_unsubscribe(connection, subscription);
      if (error != nullptr) {
        g_error_free(error);
      }
      return {false, false, "portal_request_call_failed", {}, nullptr};
    }
    const gchar *request_path = nullptr;
    g_variant_get(request.get(), "(&o)", &request_path);
    awaited.expected_path = request_path;
    if (!accept_handle(awaited.expected_path)) {
      g_dbus_connection_signal_unsubscribe(connection, subscription);
      return {false, false, "portal_lifecycle_transition_invalid", awaited.expected_path, nullptr};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (awaited.parameters == nullptr && std::chrono::steady_clock::now() < deadline &&
           terminal == std::nullopt) {
      while (g_main_context_iteration(context, FALSE) != FALSE) {
      }
      if (awaited.parameters == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (awaited.parameters == nullptr) {
      return {false, false,
              terminal == CaptureState::disconnected ? "portal_disconnected"
                                                     : "portal_response_timeout",
              awaited.expected_path, nullptr};
    }
    guint32 response_code = 2U;
    GVariant *results = nullptr;
    g_variant_get(awaited.parameters.get(), "(u@a{sv})", &response_code, &results);
    if (response_code != 0U) {
      if (results != nullptr) {
        g_variant_unref(results);
      }
      return {false, response_code == 1U,
              response_code == 1U ? "portal_selection_cancelled" : "portal_request_rejected",
              awaited.expected_path, nullptr};
    }
    return {true, false, "portal_request_complete", awaited.expected_path, VariantPtr(results)};
  }

  void subscribe_session_closed() {
    session_closed_subscription = g_dbus_connection_signal_subscribe(
        connection, kPortalName, kSessionInterface, "Closed", session_handle.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, Implementation::session_closed, this, nullptr);
  }

  void close_session() {
    if (connection == nullptr || session_handle.empty()) {
      return;
    }
    if (session_closed_subscription != 0U) {
      g_dbus_connection_signal_unsubscribe(connection, session_closed_subscription);
      session_closed_subscription = 0U;
    }
    GError *error = nullptr;
    VariantPtr ignored(g_dbus_connection_call_sync(connection, kPortalName, session_handle.c_str(),
                                                   kSessionInterface, "Close", nullptr, nullptr,
                                                   G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error));
    if (error != nullptr) {
      g_error_free(error);
    }
    session_handle.clear();
  }
};

LinuxPortalClient::LinuxPortalClient() : implementation_(std::make_unique<Implementation>()) {}
LinuxPortalClient::~LinuxPortalClient() = default;
LinuxPortalClient::LinuxPortalClient(LinuxPortalClient &&) noexcept = default;
LinuxPortalClient &LinuxPortalClient::operator=(LinuxPortalClient &&) noexcept = default;

PortalOpenResult LinuxPortalClient::open_window(std::string_view parent_window,
                                                std::uint32_t timeout_ms) {
  PortalOpenResult output;
  auto &implementation = *implementation_;
  if (!implementation.session_handle.empty() ||
      implementation.lifecycle.state() != CaptureState::idle) {
    output.reason = "portal_session_already_active";
    return output;
  }
  if (timeout_ms == 0U || timeout_ms > 300000U) {
    output.reason = "portal_timeout_invalid";
    return output;
  }
  g_main_context_push_thread_default(implementation.context);
  const auto pop_context = [&implementation]() {
    g_main_context_pop_thread_default(implementation.context);
  };
  if (!implementation.connect_bus(timeout_ms, output.reason)) {
    pop_context();
    return output;
  }
  const auto version = implementation.property("version", timeout_ms);
  const auto source_types = implementation.property("AvailableSourceTypes", timeout_ms);
  const auto cursor_modes = implementation.property("AvailableCursorModes", timeout_ms);
  if (!version || !source_types || !cursor_modes) {
    output.reason = "portal_capability_query_failed";
    pop_context();
    return output;
  }
  output.capabilities = {*version, *source_types, *cursor_modes, select_cursor_mode(*cursor_modes)};
  if ((*source_types & kSourceTypeWindow) == 0U ||
      (*cursor_modes & (kCursorHidden | kCursorEmbedded | kCursorMetadata)) == 0U) {
    output.reason = "portal_window_capture_capability_missing";
    pop_context();
    return output;
  }

  const auto create = implementation.call_request(
      "CreateSession", g_variant_new("(@a{sv})", create_options(request_token(), request_token())),
      timeout_ms, [&implementation](std::string_view handle) {
        return implementation.lifecycle.begin(std::string(handle)).passed;
      });
  if (!create.passed) {
    output.cancelled = create.cancelled;
    output.reason = create.reason;
    if (create.cancelled) {
      implementation.lifecycle.cancel(create.request_handle);
    }
    pop_context();
    return output;
  }
  const gchar *session_handle = nullptr;
  if (!g_variant_lookup(create.results.get(), "session_handle", "&o", &session_handle) ||
      session_handle == nullptr) {
    implementation.lifecycle.close(CaptureState::closed);
    output.reason = "portal_session_handle_missing";
    pop_context();
    return output;
  }
  implementation.session_handle = session_handle;
  implementation.subscribe_session_closed();

  const auto select = implementation.call_request(
      "SelectSources",
      g_variant_new("(o@a{sv})", implementation.session_handle.c_str(),
                    select_options(request_token(), output.capabilities.selected_cursor_mode)),
      timeout_ms, [&implementation, &create](std::string_view handle) {
        return implementation.lifecycle
            .session_created(create.request_handle, implementation.session_handle,
                             std::string(handle))
            .passed;
      });
  if (!select.passed) {
    output.cancelled = select.cancelled;
    output.reason = select.reason;
    if (select.cancelled) {
      implementation.lifecycle.cancel(select.request_handle);
    }
    implementation.close_session();
    pop_context();
    return output;
  }

  const auto start = implementation.call_request(
      "Start",
      g_variant_new("(os@a{sv})", implementation.session_handle.c_str(),
                    std::string(parent_window).c_str(), handle_options(request_token())),
      timeout_ms, [&implementation, &select](std::string_view handle) {
        return implementation.lifecycle.sources_selected(select.request_handle, std::string(handle))
            .passed;
      });
  if (!start.passed) {
    output.cancelled = start.cancelled;
    output.reason = start.reason;
    if (start.cancelled) {
      implementation.lifecycle.cancel(start.request_handle);
    }
    implementation.close_session();
    pop_context();
    return output;
  }
  VariantPtr streams(
      g_variant_lookup_value(start.results.get(), "streams", G_VARIANT_TYPE("a(ua{sv})")));
  if (streams == nullptr || g_variant_n_children(streams.get()) != 1U) {
    implementation.lifecycle.close(CaptureState::closed);
    implementation.close_session();
    output.reason = "portal_stream_count_invalid";
    pop_context();
    return output;
  }
  VariantPtr stream(g_variant_get_child_value(streams.get(), 0U));
  guint32 node_id = 0U;
  GVariant *stream_properties = nullptr;
  g_variant_get(stream.get(), "(u@a{sv})", &node_id, &stream_properties);
  if (stream_properties != nullptr) {
    g_variant_unref(stream_properties);
  }
  if (!implementation.lifecycle.started(start.request_handle, node_id).passed) {
    implementation.close_session();
    output.reason = "portal_pipewire_node_invalid";
    pop_context();
    return output;
  }

  GError *error = nullptr;
  GUnixFDList *output_fds = nullptr;
  VariantPtr remote(g_dbus_connection_call_with_unix_fd_list_sync(
      implementation.connection, kPortalName, kPortalPath, kScreenCastInterface,
      "OpenPipeWireRemote",
      g_variant_new("(o@a{sv})", implementation.session_handle.c_str(), empty_options()),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, static_cast<gint>(timeout_ms), nullptr,
      &output_fds, nullptr, &error));
  FdListPtr fds(output_fds);
  if (remote == nullptr || fds == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
    }
    implementation.lifecycle.close(CaptureState::closed);
    implementation.close_session();
    output.reason = "portal_pipewire_remote_failed";
    pop_context();
    return output;
  }
  gint32 fd_handle = -1;
  g_variant_get(remote.get(), "(h)", &fd_handle);
  const int remote_fd = g_unix_fd_list_get(fds.get(), fd_handle, &error);
  if (remote_fd < 0) {
    if (error != nullptr) {
      g_error_free(error);
    }
    implementation.lifecycle.close(CaptureState::closed);
    implementation.close_session();
    output.reason = "portal_pipewire_fd_invalid";
    pop_context();
    return output;
  }
  const auto descriptor_flags = fcntl(remote_fd, F_GETFD);
  if (descriptor_flags < 0 || fcntl(remote_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    ::close(remote_fd);
    implementation.lifecycle.close(CaptureState::closed);
    implementation.close_session();
    output.reason = "portal_pipewire_fd_cloexec_failed";
    pop_context();
    return output;
  }
  output.grant = PortalWindowGrant(remote_fd, node_id, output.capabilities.selected_cursor_mode);
  output.passed = true;
  output.reason = "portal_window_selected";
  pop_context();
  return output;
}

std::optional<CaptureState> LinuxPortalClient::poll_terminal() {
  auto &implementation = *implementation_;
  g_main_context_push_thread_default(implementation.context);
  while (g_main_context_iteration(implementation.context, FALSE) != FALSE) {
  }
  g_main_context_pop_thread_default(implementation.context);
  if (implementation.terminal) {
    const auto terminal = implementation.terminal;
    implementation.terminal.reset();
    implementation.lifecycle.close(*terminal);
    implementation.close_session();
    return terminal;
  }
  return std::nullopt;
}

CaptureOperationResult LinuxPortalClient::close() {
  auto &implementation = *implementation_;
  if (implementation.lifecycle.state() != CaptureState::idle &&
      implementation.lifecycle.state() != CaptureState::cancelled &&
      implementation.lifecycle.state() != CaptureState::closed &&
      implementation.lifecycle.state() != CaptureState::revoked &&
      implementation.lifecycle.state() != CaptureState::disconnected) {
    implementation.lifecycle.close(CaptureState::closed);
  }
  implementation.close_session();
  implementation.terminal.reset();
  return {true, false, "linux_capture_session_closed"};
}

bool linux_capture_backend_available() { return true; }

} // namespace glyphrelay
