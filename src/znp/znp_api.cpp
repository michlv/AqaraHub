#include "znp/znp_api.h"
#include <boost/asio/deadline_timer.hpp>
#include <sstream>
#include <stlab/concurrency/immediate_executor.hpp>
#include <stlab/concurrency/utility.hpp>
#include "logging.h"
#include "znp/encoding.h"

namespace znp {
ZnpApi::ZnpApi(boost::asio::io_service& io_service,
               std::shared_ptr<ZnpRawInterface> interface)
    : io_service_(io_service),
      raw_(std::move(interface)),
      on_frame_connection_(raw_->on_frame_.connect(
          std::bind(&ZnpApi::OnFrame, this, std::placeholders::_1,
                    std::placeholders::_2, std::placeholders::_3))) {
  AddSimpleEventHandler(ZnpCommandType::AREQ, SysCommand::RESET_IND,
                        sys_on_reset_, false);
  AddSimpleEventHandler(ZnpCommandType::AREQ, ZdoCommand::STATE_CHANGE_IND,
                        zdo_on_state_change_, false);
  AddSimpleEventHandler(ZnpCommandType::AREQ, ZdoCommand::END_DEVICE_ANNCE_IND,
                        zdo_on_end_device_announce_, false);
  AddSimpleEventHandler(ZnpCommandType::AREQ, ZdoCommand::TC_DEV_IND,
                        zdo_on_trustcenter_device_, false);
  AddSimpleEventHandler(ZnpCommandType::AREQ, ZdoCommand::PERMIT_JOIN_IND,
                        zdo_on_permit_join_, false);
  // NOTE: INCOMING_MSG sometimes has 3 extra trailing bytes, so allow a partial
  // decoding.
  AddSimpleEventHandler(ZnpCommandType::AREQ, AfCommand::INCOMING_MSG,
                        af_on_incoming_msg_, true);
}

stlab::future<ResetInfo> ZnpApi::SysReset(bool soft_reset) {
  auto package = stlab::package<ResetInfo(ResetInfo)>(
      stlab::immediate_executor, [](ResetInfo info) { return info; });
  sys_on_reset_.connect_extended(
      [package](const boost::signals2::connection& connection, ResetInfo info) {
        connection.disconnect();
        package.first(info);
      });
  raw_->SendFrame(ZnpCommandType::AREQ, SysCommand::RESET,
                  Encode<bool>(soft_reset));
  return package.second;
}

stlab::future<Capability> ZnpApi::SysPing() {
  return RawSReq(SysCommand::PING, znp::Encode()).then(znp::Decode<Capability>);
}

stlab::future<VersionInfo> ZnpApi::SysVersion() {
  return RawSReq(SysCommand::VERSION, znp::Encode()).then(znp::Decode<VersionInfo>);
}

stlab::future<void> ZnpApi::SysOsalNvItemInitRaw(
    NvItemId Id, uint16_t ItemLen, std::vector<uint8_t> InitData) {
  return RawSReq(SysCommand::OSAL_NV_ITEM_INIT,
                 znp::EncodeT(Id, ItemLen, InitData))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<std::vector<uint8_t>> ZnpApi::SysOsalNvReadRaw(NvItemId Id,
                                                             uint8_t Offset) {
  return RawSReq(SysCommand::OSAL_NV_READ, znp::EncodeT(Id, Offset))
      .then(&ZnpApi::CheckStatus)
      .then(&znp::Decode<std::vector<uint8_t>>);
}

stlab::future<void> ZnpApi::SysOsalNvWriteRaw(NvItemId Id, uint8_t Offset,
                                              std::vector<uint8_t> Value) {
  return RawSReq(SysCommand::OSAL_NV_WRITE, znp::EncodeT(Id, Offset, Value))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<void> ZnpApi::SysOsalNvDelete(NvItemId Id, uint16_t ItemLen) {
  return RawSReq(SysCommand::OSAL_NV_DELETE, znp::EncodeT(Id, ItemLen))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<uint16_t> ZnpApi::SysOsalNvLength(NvItemId Id) {
  return RawSReq(SysCommand::OSAL_NV_LENGTH, znp::EncodeT(Id))
      .then(&znp::Decode<uint16_t>);
}

stlab::future<void> ZnpApi::AfRegister(uint8_t endpoint, uint16_t profile_id,
                                       uint16_t device_id, uint8_t version,
                                       Latency latency,
                                       std::vector<uint16_t> input_clusters,
                                       std::vector<uint16_t> output_clusters) {
  return RawSReq(AfCommand::REGISTER,
                 znp::EncodeT(endpoint, profile_id, device_id, version, latency,
                              input_clusters, output_clusters))
      .then(CheckOnlyStatus);
}

stlab::future<void> ZnpApi::AfDataRequest(ShortAddress DstAddr,
                                          uint8_t DstEndpoint,
                                          uint8_t SrcEndpoint,
                                          uint16_t ClusterId, uint8_t TransId,
                                          uint8_t Options, uint8_t Radius,
                                          std::vector<uint8_t> Data) {
  return WaitAfter(
             RawSReq(AfCommand::DATA_REQUEST,
                     znp::EncodeT(DstAddr, DstEndpoint, SrcEndpoint, ClusterId,
                                  TransId, Options, Radius, Data))
                 .then(CheckOnlyStatus),
             ZnpCommandType::AREQ, AfCommand::DATA_CONFIRM)
      .then(&CheckStatus)
      .then(&znp::DecodeT<uint8_t, uint8_t>)
      .then([DstEndpoint,
             TransId](const std::tuple<uint8_t, uint8_t>& response) {
        // TODO: I would love some better management of request/response, e.g.
        // matching the endpoint and transid instead of only the DATA_CONFIRM
        // message.
        if (std::make_tuple(DstEndpoint, TransId) != response) {
          LOG("ZnpApi", warning)
              << "AF_DATA_REQUEST & AF_DATA_CONFIRM synchronization mismatch!";
          throw std::runtime_error(
              "AF_DATA_REQUEST & AF_DATA_CONFIRM synchronization mismatch!");
        }
      });
  ;
}

stlab::future<StartupFromAppResponse> ZnpApi::ZdoStartupFromApp(
    uint16_t start_delay_ms) {
  return RawSReq(ZdoCommand::STARTUP_FROM_APP, znp::Encode(start_delay_ms))
      .then(&znp::Decode<StartupFromAppResponse>);
}
stlab::future<ShortAddress> ZnpApi::ZdoMgmtLeave(ShortAddress DstAddr,
                                                 IEEEAddress DeviceAddr,
                                                 uint8_t remove_rejoin) {
  return WaitAfter(RawSReq(ZdoCommand::MGMT_LEAVE_REQ,
                           znp::EncodeT(DstAddr, DeviceAddr, remove_rejoin))
                       .then(&ZnpApi::CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::MGMT_LEAVE_RSP)
      .then(&znp::DecodeT<ShortAddress, ZnpStatus>)
      .then([](std::tuple<ShortAddress, ZnpStatus> retval) {
        if (std::get<1>(retval) != ZnpStatus::Success) {
          throw std::runtime_error("MgmtLeave returned non-success status");
        }
        return std::get<0>(retval);
      });
}
stlab::future<uint16_t> ZnpApi::ZdoMgmtDirectJoin(uint16_t DstAddr,
                                                  IEEEAddress DeviceAddress) {
  return WaitAfter(RawSReq(ZdoCommand::MGMT_DIRECT_JOIN_REQ,
                           znp::EncodeT(DstAddr, DeviceAddress))
                       .then(CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::MGMT_DIRECT_JOIN_RSP)
      .then(znp::DecodeT<uint16_t, ZnpStatus>)
      .then([](std::tuple<uint16_t, ZnpStatus> retval) {
        if (std::get<1>(retval) != ZnpStatus::Success) {
          throw std::runtime_error("DirectJoin returned non-success status");
        }
        return std::get<0>(retval);
      });
}
stlab::future<uint16_t> ZnpApi::ZdoMgmtPermitJoin(AddrMode addr_mode,
                                                  uint16_t dst_address,
                                                  uint8_t duration,
                                                  uint8_t tc_significance) {
  return WaitAfter(RawSReq(ZdoCommand::MGMT_PERMIT_JOIN_REQ,
                           znp::EncodeT(addr_mode, dst_address, duration,
                                        tc_significance))
                       .then(CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::MGMT_PERMIT_JOIN_RSP)
      .then(znp::DecodeT<uint16_t, ZnpStatus>)
      .then([](std::tuple<uint16_t, ZnpStatus> retval) {
        if (std::get<1>(retval) != ZnpStatus::Success) {
          throw std::runtime_error("PermitJoin returned non-success status");
        }
        return std::get<0>(retval);
      });
}

stlab::future<ZdoIEEEAddressResponse> ZnpApi::ZdoIEEEAddress(
    ShortAddress address, boost::optional<uint8_t> children_index) {
  return WaitAfter(RawSReq(ZdoCommand::IEEE_ADDR_REQ,
                           znp::EncodeT<ShortAddress, bool, uint8_t>(
                               address, !!children_index,
                               children_index ? *children_index : 0))
                       .then(&ZnpApi::CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::IEEE_ADDR_RSP)
      .then(&ZnpApi::CheckStatus)
      .then(&znp::Decode<ZdoIEEEAddressResponse>);
}

stlab::future<void> ZnpApi::ZdoRemoveLinkKey(IEEEAddress IEEEAddr) {
  return RawSReq(ZdoCommand::REMOVE_LINK_KEY, znp::Encode(IEEEAddr))
      .then(&ZnpApi::CheckOnlyStatus);
}
stlab::future<std::tuple<IEEEAddress, std::array<uint8_t, 16>>>
ZnpApi::ZdoGetLinkKey(IEEEAddress IEEEAddr) {
  return RawSReq(ZdoCommand::GET_LINK_KEY, znp::Encode(IEEEAddr))
      .then(&ZnpApi::CheckStatus)
      .then(&znp::Decode<std::tuple<IEEEAddress, std::array<uint8_t, 16>>>);
}

stlab::future<void> ZnpApi::ZdoBind(ShortAddress DstAddr,
                                    IEEEAddress SrcAddress, uint8_t SrcEndpoint,
                                    uint16_t ClusterId, BindTarget Dst) {
  return WaitAfter(RawSReq(ZdoCommand::BIND_REQ,
                           znp::EncodeT(DstAddr, SrcAddress, SrcEndpoint,
                                        ClusterId, Dst))
                       .then(&ZnpApi::CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::BIND_RSP, 15,
                   znp::Encode(DstAddr))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<void> ZnpApi::ZdoUnbind(ShortAddress DstAddr,
                                      IEEEAddress SrcAddress,
                                      uint8_t SrcEndpoint, uint16_t ClusterId,
                                      BindTarget Dst) {
  return WaitAfter(RawSReq(ZdoCommand::UNBIND_REQ,
                           znp::EncodeT(DstAddr, SrcAddress, SrcEndpoint,
                                        ClusterId, Dst))
                       .then(&ZnpApi::CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::UNBIND_RSP, 15,
                   znp::Encode(DstAddr))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<std::tuple<uint8_t, uint8_t, std::vector<BindTableEntry>>>
ZnpApi::ZdoMgmtBindReq(ShortAddress DstAddr, uint8_t StartIndex) {
  return WaitAfter(RawSReq(ZdoCommand::MGMT_BIND_REQ,
                           znp::EncodeT(DstAddr, StartIndex))
                       .then(&ZnpApi::CheckOnlyStatus),
                   ZnpCommandType::AREQ, ZdoCommand::MGMT_BIND_RSP, 15,
                   znp::Encode(DstAddr))
      .then(&ZnpApi::CheckStatus)
      .then(&znp::DecodeT<uint8_t, uint8_t, std::vector<BindTableEntry>>);
}

stlab::future<void> ZnpApi::ZdoExtRemoveGroup(uint8_t Endpoint,
                                              uint16_t GroupID) {
  return RawSReq(ZdoCommand::EXT_REMOVE_GROUP, znp::EncodeT(Endpoint, GroupID))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<void> ZnpApi::ZdoExtRemoveAllGroup(uint8_t Endpoint) {
  return RawSReq(ZdoCommand::EXT_REMOVE_ALL_GROUP,
                 std::set<ZnpCommand>{ZdoCommand::EXT_REMOVE_ALL_GROUP,
                                      ZdoCommand::EXT_REMOVE_GROUP},
                 znp::EncodeT(Endpoint))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<std::vector<uint16_t>> ZnpApi::ZdoExtFindAllGroupsEndpoint(
    uint8_t Endpoint) {
  return RawSReq(ZdoCommand::EXT_FIND_ALL_GROUPS_ENDPOINT,
                 znp::EncodeT(Endpoint, (uint8_t)0))
      .then(&znp::Decode<std::vector<uint16_t>>);
}

stlab::future<std::string> ZnpApi::ZdoExtFindGroup(uint8_t Endpoint,
                                                   uint16_t GroupID) {
  return RawSReq(ZdoCommand::EXT_FIND_GROUP, znp::EncodeT(Endpoint, GroupID))
      .then(&ZnpApi::CheckStatus)
      .then([GroupID](const std::vector<uint8_t>& result) -> std::string {
        uint16_t ReceiveGroupId;
        std::vector<uint8_t> GroupName;
        std::tie(ReceiveGroupId, GroupName) =
            znp::DecodePartialT<uint16_t, std::vector<uint8_t>>(result);
        if (ReceiveGroupId != GroupID) {
          throw std::runtime_error(
              "Received GroupID did not match requested GroupID");
        }
        return std::string(GroupName.begin(), GroupName.end());
      });
}

stlab::future<void> ZnpApi::ZdoExtAddGroup(uint8_t Endpoint, uint16_t GroupID,
                                           std::string GroupName) {
  std::vector<uint8_t> GroupNameVec(GroupName.begin(), GroupName.end());
  if (GroupNameVec.size() > 16) {
    throw std::runtime_error("Group name is too long");
  }
  return RawSReq(ZdoCommand::EXT_ADD_GROUP,
                 znp::EncodeT(Endpoint, GroupID, GroupNameVec))
      .then(&ZnpApi::CheckOnlyStatus);
}

stlab::future<uint8_t> ZnpApi::ZdoExtCountAllGroups() {
  return RawSReq(ZdoCommand::EXT_COUNT_ALL_GROUPS, std::vector<uint8_t>())
      .then(&znp::Decode<uint8_t>);
}

stlab::future<std::vector<uint8_t>> ZnpApi::SapiReadConfigurationRaw(
    ConfigurationOption option) {
  return RawSReq(SapiCommand::READ_CONFIGURATION, znp::Encode(option))
      .then(&CheckStatus)
      .then(&znp::Decode<std::tuple<ConfigurationOption, std::vector<uint8_t>>>)
      .then([option](const std::tuple<ConfigurationOption,
                                      std::vector<uint8_t>>& retval) {
        if (option != std::get<0>(retval)) {
          throw std::runtime_error("Read configuration returned wrong option");
        }
        return std::get<1>(retval);
      });
}

stlab::future<void> ZnpApi::SapiWriteConfigurationRaw(
    ConfigurationOption option, const std::vector<uint8_t>& value) {
  return RawSReq(SapiCommand::WRITE_CONFIGURATION, znp::EncodeT(option, value))
      .then(&CheckOnlyStatus);
}

stlab::future<std::vector<uint8_t>> ZnpApi::SapiGetDeviceInfoRaw(
    DeviceInfo info) {
  return RawSReq(SapiCommand::GET_DEVICE_INFO, znp::EncodeT(info))
      .then([info](const std::vector<uint8_t>& retval) {
        if (retval.size() < 1) {
          throw std::runtime_error(
              "Expected more data from GetDeviceInfo response");
        }
        if ((DeviceInfo)retval[0] != info) {
          throw std::runtime_error("Wrong DeviceInfo returned");
        }
        return std::vector<uint8_t>(retval.begin() + 1, retval.end());
      });
}

stlab::future<IEEEAddress> ZnpApi::UtilAddrmgrNwkAddrLookup(
    ShortAddress address) {
  return RawSReq(UtilCommand::ADDRMGR_NWK_ADDR_LOOKUP, znp::Encode(address))
      .then(&znp::Decode<IEEEAddress>);
}

stlab::future<ShortAddress> ZnpApi::UtilAddrmgrExtAddrLookup(
    IEEEAddress address) {
  return RawSReq(UtilCommand::ADDRMGR_EXT_ADDR_LOOKUP, znp::Encode(address))
      .then(&znp::Decode<ShortAddress>);
}

void ZnpApi::OnFrame(ZnpCommandType type, ZnpCommand command,
                     const std::vector<uint8_t>& payload) {
  for (auto it = handlers_.begin(); it != handlers_.end();) {
    auto action = (*it)(type, command, payload);
    if (action.remove_me) {
      it = handlers_.erase(it);
    } else {
      it++;
    }
    if (action.stop_processing) {
      return;
    }
  }
  LOG("ZnpApi", debug) << "Unhandled frame " << type << " " << command;
}

stlab::future<DeviceState> ZnpApi::WaitForState(
    std::set<DeviceState> end_states, std::set<DeviceState> allowed_states) {
  // TODO: Fix lifetime issues here with passing 'this'. Maybe shared_from_this
  // ?
  return SapiGetDeviceInfo<DeviceInfo::DeviceState>().then(
      [end_states, allowed_states, this](DeviceState state) {
        if (end_states.count(state) != 0) {
          LOG("WaitForState", debug) << "Immediately reached end state";
          return stlab::make_ready_future(state, stlab::immediate_executor);
        }
        if (allowed_states.count(state) == 0) {
          LOG("WaitForState", debug) << "Immediately reached non-allowed state";
          throw std::runtime_error("Invalid state reached");
        }
        LOG("WaitForState", debug) << "Subscribing to on_state_change_ event";
        stlab::future<DeviceState> future;
        stlab::packaged_task<std::exception_ptr, DeviceState> promise;
        std::tie(promise, future) =
            stlab::package<DeviceState(std::exception_ptr, DeviceState)>(
                stlab::immediate_executor,
                [](std::exception_ptr exc, DeviceState state) {
                  if (exc) {
                    std::rethrow_exception(exc);
                  }
                  return state;
                });
        this->zdo_on_state_change_.connect_extended(
            [promise, end_states, allowed_states](
                const boost::signals2::connection& connection,
                DeviceState state) {
              LOG("WaitForState", debug) << "Got on_state_change_";
              if (end_states.count(state) != 0) {
                connection.disconnect();
                promise(nullptr, state);
                return;
              }
              if (allowed_states.count(state) == 0) {
                connection.disconnect();
                promise(std::make_exception_ptr(
                            std::runtime_error("Non-allowed state reached")),
                        DeviceState());
                return;
              }
            });

        return future;
      });
}

stlab::future<std::vector<uint8_t>> ZnpApi::WaitFor(
    ZnpCommandType type, ZnpCommand command, int timeout_in_seconds,
    std::vector<uint8_t> data_prefix) {
  auto package = stlab::package<std::vector<uint8_t>(std::exception_ptr,
                                                     std::vector<uint8_t>)>(
      stlab::immediate_executor,
      [](std::exception_ptr exc, std::vector<uint8_t> retval) {
        if (exc != nullptr) {
          std::rethrow_exception(exc);
        }
        return retval;
      });
  AddHandlerWithTimeout(
      timeout_in_seconds,
      [promise{package.first}, type, command,
       data_prefix{std::move(data_prefix)}](
          const ZnpCommandType& recvd_type, const ZnpCommand& recvd_command,
          const std::vector<uint8_t>& data) -> FrameHandlerAction {
        if (recvd_type == type && recvd_command == command &&
            data.size() >= data_prefix.size() &&
            memcmp(&data[0], &data_prefix[0], data_prefix.size()) == 0) {
          if (data_prefix.size() == 0) {
            promise(nullptr, data);
          } else {
            promise(nullptr,
                    std::vector<uint8_t>(data.cbegin() + data_prefix.size(),
                                         data.cend()));
          }
          return {true, true};
        }
        return {false, false};
      },
      [promise{package.first}]() {
        promise(std::make_exception_ptr(std::runtime_error("Timeout")),
                std::vector<uint8_t>());
      });
  return package.second;
}

stlab::future<std::vector<uint8_t>> ZnpApi::WaitAfter(
    stlab::future<void> first_request, ZnpCommandType type, ZnpCommand command,
    int timeout_in_seconds, std::vector<uint8_t> data_prefix) {
  // TODO: Fix lifetime issues here!
  auto f = first_request.then([this, type, command, timeout_in_seconds,
                               data_prefix{std::move(data_prefix)}]() {
    return this->WaitFor(type, command, timeout_in_seconds,
                         std::move(data_prefix));
  });
  f.detach();
  return f;
}

stlab::future<std::vector<uint8_t>> ZnpApi::RawSReq(
    ZnpCommand command, const std::vector<uint8_t>& payload) {
  return RawSReq(command, std::set<ZnpCommand>{command}, payload);
}

stlab::future<std::vector<uint8_t>> ZnpApi::RawSReq(
    ZnpCommand command, std::set<ZnpCommand> possible_responses,
    const std::vector<uint8_t>& payload) {
  auto package = stlab::package<std::vector<uint8_t>(std::exception_ptr,
                                                     std::vector<uint8_t>)>(
      stlab::immediate_executor,
      [](std::exception_ptr ex, std::vector<uint8_t> data) {
        if (ex != nullptr) {
          std::rethrow_exception(ex);
        }
        return data;
      });
  handlers_.push_back([package, possible_responses](
                          const ZnpCommandType& type,
                          const ZnpCommand& recvd_command,
                          const std::vector<uint8_t>& data)
                          -> FrameHandlerAction {
    // Normal response
    if (type == ZnpCommandType::SRSP &&
        possible_responses.find(recvd_command) != possible_responses.end()) {
      package.first(nullptr, data);
      return {true, true};
    }
    // Possible RPC_Error response
    if (type == ZnpCommandType::SRSP &&
        recvd_command == ZnpCommand(ZnpSubsystem::RPC_Error, 0)) {
      try {
        auto info = znp::DecodeT<uint8_t, uint8_t, uint8_t>(data);
        ZnpCommand err_command((ZnpSubsystem)(std::get<1>(info) & 0xF),
                               std::get<2>(info));
        ZnpCommandType err_type = (ZnpCommandType)(std::get<1>(info) >> 4);
        if (err_type == ZnpCommandType::SREQ &&
            possible_responses.find(err_command) != possible_responses.end()) {
          std::stringstream ss;
          ss << "RPC Error: " << (unsigned int)std::get<0>(info);
          package.first(std::make_exception_ptr(std::runtime_error(ss.str())),
                        std::vector<uint8_t>());
          return {true, true};
        }
      } catch (const std::exception& exc) {
        LOG("ZnpApi", debug) << "Unable to parse RPCError";
      }
    }
    return {false, false};
  });
  raw_->SendFrame(ZnpCommandType::SREQ, command, payload);
  return package.second;
}

/**
 * Handler will be called like normal, until the timeout expires, or if it
 * returns it should be removed. Timeout handler will be called when the timeout
 * expires, and the handler hasn't been removed yet.
 */
void ZnpApi::AddHandlerWithTimeout(int timeout_in_seconds, FrameHandler handler,
                                   TimeoutHandler timeout_handler) {
  if (timeout_in_seconds <= 0) {
    handlers_.push_back(handler);
    return;
  }
  struct HandlerTimeoutInfo {
    bool active;
    boost::asio::deadline_timer timer;

    HandlerTimeoutInfo(boost::asio::io_service& io_service)
        : timer(io_service) {
      active = true;
    }
  };
  auto shared_info = std::make_shared<HandlerTimeoutInfo>(io_service_);
  shared_info->timer.expires_from_now(
      boost::posix_time::seconds(timeout_in_seconds));
  shared_info->timer.async_wait(
      [shared_info, timeout_handler](const boost::system::error_code& ec) {
        if (!shared_info->active) {
          return;
        }
        shared_info->active = false;
        timeout_handler();
      });
  handlers_.push_back(
      [shared_info, handler](
          const ZnpCommandType& type, const ZnpCommand& cmd,
          const std::vector<uint8_t>& data) -> FrameHandlerAction {
        if (!shared_info->active) {
          return {false, true};
        }
        FrameHandlerAction action = handler(type, cmd, data);
        if (action.remove_me) {
          shared_info->active = false;
        }
        return action;
      });
}

std::vector<uint8_t> ZnpApi::CheckStatus(const std::vector<uint8_t>& response) {
  if (response.size() < 1) {
    throw std::runtime_error("Empty response received");
  }
  if (response[0] != (uint8_t)ZnpStatus::Success) {
    // TODO: Parse and throw proper error!
    throw std::runtime_error("ZNP Status was not success");
  }
  return std::vector<uint8_t>(response.begin() + 1, response.end());
}

void ZnpApi::CheckOnlyStatus(const std::vector<uint8_t>& response) {
  if (CheckStatus(response).size() != 0) {
    throw std::runtime_error("Empty response after status expected");
  }
}

}  // namespace znp
