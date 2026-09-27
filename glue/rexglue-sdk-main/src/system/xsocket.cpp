/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex::system {

namespace {

bool ToNativeIpv4(const N_XSOCKADDR_IN& xbox_address, int xbox_address_length,
                  sockaddr_in& native_address) {
  if (xbox_address_length < static_cast<int>(sizeof(N_XSOCKADDR_IN)) ||
      xbox_address.sin_family != XSocket::X_AF_INET) {
    return false;
  }
  native_address = {};
#if REX_PLATFORM_MAC
  native_address.sin_len = sizeof(native_address);
#endif
  native_address.sin_family = AF_INET;
  native_address.sin_port = htons(static_cast<uint16_t>(xbox_address.sin_port));
  native_address.sin_addr.s_addr =
      htonl(static_cast<uint32_t>(xbox_address.sin_addr));
  return true;
}

bool FromNativeIpv4(const sockaddr_in& native_address,
                    N_XSOCKADDR_IN& xbox_address) {
  if (native_address.sin_family != AF_INET) return false;
  xbox_address.sin_family = XSocket::X_AF_INET;
  xbox_address.sin_port = ntohs(native_address.sin_port);
  xbox_address.sin_addr = ntohl(native_address.sin_addr.s_addr);
  std::memset(xbox_address.x_sin_zero, 0, sizeof(xbox_address.x_sin_zero));
  return true;
}

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == static_cast<uint64_t>(rex::net::kInvalidSocket)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (native_handle_ == static_cast<uint64_t>(rex::net::kInvalidSocket)) {
    return X_STATUS_SUCCESS;
  }

  const uint64_t handle = native_handle_;
  native_handle_ = static_cast<uint64_t>(rex::net::kInvalidSocket);
  bound_ = false;
  bound_port_ = 0;
  int ret = rex::net::socket_close(handle);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (!name) return X_STATUS_INVALID_PARAMETER;
  N_XSOCKADDR_IN xbox_address{};
  std::memcpy(&xbox_address, name, sizeof(xbox_address));
  sockaddr_in native_address{};
  if (!ToNativeIpv4(xbox_address, name_len, native_address)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const int ret =
      connect(native_handle_, reinterpret_cast<const sockaddr*>(&native_address),
              sizeof(native_address));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (!name) return X_STATUS_INVALID_PARAMETER;
  sockaddr_in native_address{};
  if (!ToNativeIpv4(*name, name_len, native_address)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const int ret =
      bind(native_handle_, reinterpret_cast<const sockaddr*>(&native_address),
           sizeof(native_address));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  N_XSOCKADDR_IN bound_name{};
  int bound_name_len = sizeof(bound_name);
  if (GetSockName(reinterpret_cast<N_XSOCKADDR*>(&bound_name), &bound_name_len) == 0 &&
      bound_name_len >= static_cast<int>(sizeof(bound_name))) {
    // A zero-port bind asks the host OS to select an ephemeral port. Peer
    // datagram routing must use that selected port, not the zero in the bind
    // request. The wrapper keeps the bytes in Xbox network order while its
    // value conversion yields the numeric port used by peer routing.
    bound_port_ = static_cast<uint16_t>(bound_name.sin_port);
  } else {
    bound_port_ = static_cast<uint16_t>(name->sin_port);
  }

  return X_STATUS_SUCCESS;
}

int XSocket::GetSockName(N_XSOCKADDR* name, int* name_len) {
  if (!name || !name_len ||
      *name_len < static_cast<int>(sizeof(N_XSOCKADDR_IN))) {
    return -1;
  }

  sockaddr_in native_name{};
  socklen_t native_name_len = sizeof(native_name);
  const int ret = getsockname(native_handle_, reinterpret_cast<sockaddr*>(&native_name),
                              &native_name_len);
  if (ret < 0) {
    return ret;
  }
  if (native_name_len < sizeof(native_name)) {
    return -1;
  }

  N_XSOCKADDR_IN xbox_name{};
  if (!FromNativeIpv4(native_name, xbox_name)) return -1;
  std::memcpy(name, &xbox_name, sizeof(xbox_name));
  *name_len = sizeof(xbox_name);
  return 0;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  if ((name && (!name_len ||
                *name_len < static_cast<int>(sizeof(N_XSOCKADDR_IN)))) ||
      (!name && name_len)) {
    return nullptr;
  }
  sockaddr_in native_name{};
  socklen_t native_name_len = sizeof(native_name);
  const uintptr_t ret = accept(
      native_handle_, name ? reinterpret_cast<sockaddr*>(&native_name) : nullptr,
      name ? &native_name_len : nullptr);
  if (ret == static_cast<uintptr_t>(rex::net::kInvalidSocket)) {
    if (name) std::memset(name, 0, *name_len);
    if (name_len) *name_len = 0;
    return nullptr;
  }

  if (name) {
    N_XSOCKADDR_IN xbox_name{};
    if (!FromNativeIpv4(native_name, xbox_name)) {
      rex::net::socket_close(ret);
      std::memset(name, 0, *name_len);
      *name_len = 0;
      return nullptr;
    }
    std::memcpy(name, &xbox_name, sizeof(xbox_name));
    *name_len = sizeof(xbox_name);
  }

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  auto* live = kernel_state_ ? kernel_state_->live_compatibility() : nullptr;
  if (type_ == X_SOCK_DGRAM && live && live->peer_transport()) {
    auto datagram = live->ReceivePeerDatagram(bound_port_, buf_len);
    if (datagram) {
      const uint32_t copied_size =
          std::min(buf_len, static_cast<uint32_t>(datagram->payload.size()));
      std::memcpy(buf, datagram->payload.data(), copied_size);
      if (from) {
        from->sin_family = X_AF_INET;
        from->sin_addr = ntohl(datagram->source_ipv4);
        from->sin_port = datagram->source_port;
        std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
      }
      if (from_len) {
        *from_len = sizeof(N_XSOCKADDR_IN);
      }
      return static_cast<int>(copied_size);
    }
  }

  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom{};
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (ret >= 0 && from) {
    if (!FromNativeIpv4(nfrom, *from)) return -1;
  }

  if (ret >= 0 && from_len) {
    *from_len = sizeof(N_XSOCKADDR_IN);
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

bool XSocket::UsesPeerDatagramTransport() const {
  auto* live = kernel_state_ ? kernel_state_->live_compatibility() : nullptr;
  return type_ == X_SOCK_DGRAM && live && live->peer_transport();
}

bool XSocket::HasPendingPeerDatagram() const {
  auto* live = kernel_state_ ? kernel_state_->live_compatibility() : nullptr;
  return type_ == X_SOCK_DGRAM && live && live->HasPendingPeerDatagram(bound_port_);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  auto* live = kernel_state_ ? kernel_state_->live_compatibility() : nullptr;
  if (type_ == X_SOCK_DGRAM && to && live && live->peer_transport()) {
    const uint32_t destination_ipv4 = htonl(static_cast<uint32_t>(to->sin_addr));
    if (live->FindRoute(destination_ipv4)) {
      return live->SendPeerDatagram(destination_ipv4, to->sin_port, bound_port_,
                                    std::span<const uint8_t>(buf, buf_len))
                 ? static_cast<int>(buf_len)
                 : -1;
    }
  }

  sockaddr_in nto{};
  if (to &&
      (to_len < sizeof(N_XSOCKADDR_IN) ||
       !ToNativeIpv4(*to, sizeof(N_XSOCKADDR_IN), nto))) {
    return -1;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? reinterpret_cast<sockaddr*>(&nto) : nullptr,
                to ? sizeof(nto) : 0);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
