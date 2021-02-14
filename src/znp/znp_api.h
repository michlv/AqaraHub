#ifndef _ZNP_API_H_
#define _ZNP_API_H_
#include <bitset>
#include <boost/asio/io_service.hpp>
#include <boost/signals2/signal.hpp>
#include <map>
#include <queue>
#include <set>
#include <stlab/concurrency/future.hpp>
#include <vector>
#include "logging.h"
#include "polyfill/apply.h"
#include "znp/encoding.h"
#include "znp/znp.h"
#include "znp/znp_raw_interface.h"

namespace znp {
class ZnpApi {
 public:
  ZnpApi(boost::asio::io_service& io_service,
         std::shared_ptr<ZnpRawInterface> interface);
  ~ZnpApi() = default;

  // SYS commands
  stlab::future<ResetInfo> SysReset(bool soft_reset);
  stlab::future<Capability> SysPing();
  stlab::future<VersionInfo> SysVersion();
  stlab::future<void> SysOsalNvItemInitRaw(NvItemId Id, uint16_t ItemLen,
                                           std::vector<uint8_t> InitData);
  stlab::future<std::vector<uint8_t>> SysOsalNvReadRaw(NvItemId Id,
                                                       uint8_t Offset);
  stlab::future<void> SysOsalNvWriteRaw(NvItemId Id, uint8_t Offset,
                                        std::vector<uint8_t> Value);
  stlab::future<void> SysOsalNvDelete(NvItemId Id, uint16_t ItemLen);
  stlab::future<uint16_t> SysOsalNvLength(NvItemId Id);

  // SYS events
  boost::signals2::signal<void(ResetInfo)> sys_on_reset_;

  // AF commands
  stlab::future<void> AfRegister(uint8_t endpoint, uint16_t profile_id,
                                 uint16_t device_id, uint8_t version,
                                 Latency latency,
                                 std::vector<uint16_t> input_clusters,
                                 std::vector<uint16_t> output_clusters);
  stlab::future<void> AfDataRequest(ShortAddress DstAddr, uint8_t DstEndpoint,
                                    uint8_t SrcEndpoint, uint16_t ClusterId,
                                    uint8_t TransId, uint8_t Options,
                                    uint8_t Radius, std::vector<uint8_t> Data);
  // AF events
  boost::signals2::signal<void(const IncomingMsg&)> af_on_incoming_msg_;

  // ZDO commands
  stlab::future<ZdoIEEEAddressResponse> ZdoIEEEAddress(
      ShortAddress address, boost::optional<uint8_t> children_index);
  stlab::future<void> ZdoRemoveLinkKey(IEEEAddress IEEEAddr);
  stlab::future<std::tuple<IEEEAddress, std::array<uint8_t, 16>>> ZdoGetLinkKey(
      IEEEAddress IEEEAddr);
  stlab::future<ShortAddress> ZdoMgmtLeave(ShortAddress DstAddr,
                                           IEEEAddress DeviceAddr,
                                           uint8_t remove_rejoin);
  stlab::future<uint16_t> ZdoMgmtDirectJoin(uint16_t DstAddr,
                                            IEEEAddress DeviceAddress);
  stlab::future<uint16_t> ZdoMgmtPermitJoin(AddrMode addr_mode,
                                            uint16_t dst_address,
                                            uint8_t duration,
                                            uint8_t tc_significance);

  stlab::future<StartupFromAppResponse> ZdoStartupFromApp(
      uint16_t start_delay_ms);

  stlab::future<void> ZdoNodeDescReq(ShortAddress address);
  stlab::future<void> ZdoActiveEpReq(ShortAddress address);
  stlab::future<void> ZdoSimpleDescReq(ShortAddress address, uint8_t EndPoint);

  stlab::future<void> ZdoBind(ShortAddress DstAddr, IEEEAddress SrcAddress,
                              uint8_t SrcEndpoint, uint16_t ClusterId,
                              BindTarget target);
  stlab::future<void> ZdoUnbind(ShortAddress DstAddr, IEEEAddress SrcAddress,
                                uint8_t SrcEndpoint, uint16_t ClusterId,
                                BindTarget target);
  stlab::future<std::tuple<uint8_t, uint8_t, std::vector<BindTableEntry>>>
  ZdoMgmtBindReq(ShortAddress DstAddr, uint8_t StartIndex);
  stlab::future<void> ZdoExtRemoveGroup(uint8_t Endpoint, uint16_t GroupID);
  stlab::future<void> ZdoExtRemoveAllGroup(uint8_t Endpoint);
  stlab::future<std::vector<uint16_t>> ZdoExtFindAllGroupsEndpoint(
      uint8_t Endpoint);
  stlab::future<std::string> ZdoExtFindGroup(uint8_t Endpoint,
                                             uint16_t GroupID);
  stlab::future<void> ZdoExtAddGroup(uint8_t Endpoint, uint16_t GroupID,
                                     std::string GroupName);
  stlab::future<uint8_t> ZdoExtCountAllGroups();

  // ZDO events
  boost::signals2::signal<void(DeviceState)> zdo_on_state_change_;
  boost::signals2::signal<void(ShortAddress, IEEEAddress, ShortAddress)>
      zdo_on_trustcenter_device_;
  boost::signals2::signal<void(ShortAddress, ShortAddress, IEEEAddress,
                               uint8_t)>
      zdo_on_end_device_announce_;
  boost::signals2::signal<void(ShortAddress, IEEEAddress,
                               uint8_t, uint8_t, uint8_t)>
      zdo_on_leave_ind_;
  boost::signals2::signal<void(uint8_t)> zdo_on_permit_join_;

  boost::signals2::signal<void(NodeDescRsp)> zdo_on_node_desc_;
  boost::signals2::signal<void(ActiveEpRsp)> zdo_on_active_ep_;
  boost::signals2::signal<void(SimpleDescRsp)> zdo_on_simple_desc_;

  // SAPI commands
  stlab::future<std::vector<uint8_t>> SapiReadConfigurationRaw(
      ConfigurationOption option);
  template <ConfigurationOption O>
  stlab::future<typename ConfigurationOptionInfo<O>::Type>
  SapiReadConfiguration() {
    return SapiReadConfigurationRaw(O).then(
        &znp::Decode<typename ConfigurationOptionInfo<O>::Type>);
  }
  stlab::future<void> SapiWriteConfigurationRaw(
      ConfigurationOption option, const std::vector<uint8_t>& value);
  template <ConfigurationOption O>
  stlab::future<void> SapiWriteConfiguration(
      const typename ConfigurationOptionInfo<O>::Type& value) {
    return SapiWriteConfigurationRaw(O, znp::Encode(value));
  }

  stlab::future<std::vector<uint8_t>> SapiGetDeviceInfoRaw(DeviceInfo info);
  template <DeviceInfo I>
  stlab::future<typename DeviceInfoInfo<I>::Type> SapiGetDeviceInfo() {
    // DecodePartial because GetDeviceInfo will always return 8 bytes, even if
    // less are needed.
    return SapiGetDeviceInfoRaw(I).then(
        &znp::DecodePartial<typename DeviceInfoInfo<I>::Type>);
  }

  // SAPI events

  // UTIL commands
  stlab::future<IEEEAddress> UtilAddrmgrNwkAddrLookup(ShortAddress address);
  stlab::future<ShortAddress> UtilAddrmgrExtAddrLookup(IEEEAddress address);
  // UTIL events

  // APP_CNF commands
  stlab::future<void> AppCnfBdbSetChannel(bool isPrimary, uint32_t channelMask);
  stlab::future<void> AppCnfBdbStartCommissioning(uint8_t mode);

  // APP_CNF events
  boost::signals2::signal<void(uint8_t, uint8_t, uint8_t)> app_cnf_on_bdb_commissioning_notification_;

  // Helper functions
  stlab::future<DeviceState> WaitForState(std::set<DeviceState> end_states,
                                          std::set<DeviceState> allowed_states);

 private:
  boost::asio::io_service& io_service_;
  std::shared_ptr<ZnpRawInterface> raw_;
  boost::signals2::scoped_connection on_frame_connection_;

  struct FrameHandlerAction {
    bool
        stop_processing;  // If true, do not call handlers further down the list
    bool remove_me;  // If true, remove this handler from the list, and do not
                     // call again.
  };
  typedef std::function<FrameHandlerAction(
      const ZnpCommandType&, const ZnpCommand&, const std::vector<uint8_t>&)>
      FrameHandler;
  std::list<FrameHandler> handlers_;

  void OnFrame(ZnpCommandType type, ZnpCommand command,
               const std::vector<uint8_t>& payload);
  stlab::future<std::vector<uint8_t>> WaitFor(
      ZnpCommandType type, ZnpCommand command, int timeout_in_seconds = 0,
      std::vector<uint8_t> data_prefix = std::vector<uint8_t>());
  stlab::future<std::vector<uint8_t>> WaitAfter(
      stlab::future<void> first_request, ZnpCommandType type,
      ZnpCommand command, int timeout_in_seconds = 0,
      std::vector<uint8_t> data_prefix = std::vector<uint8_t>());
  stlab::future<std::vector<uint8_t>> RawSReq(
      ZnpCommand command, const std::vector<uint8_t>& payload);
  stlab::future<std::vector<uint8_t>> RawSReq(
      ZnpCommand command, std::set<ZnpCommand> possible_responses,
      const std::vector<uint8_t>& payload);
  static std::vector<uint8_t> CheckStatus(const std::vector<uint8_t>& response);
  static void CheckOnlyStatus(const std::vector<uint8_t>& response);
  typedef std::function<void()> TimeoutHandler;
  void AddHandlerWithTimeout(int timeout_in_seconds, FrameHandler handler,
                             TimeoutHandler timeout_handler);

  template <typename... Args>
  void AddSimpleEventHandler(ZnpCommandType type, ZnpCommand command,
                             boost::signals2::signal<void(Args...)>& signal,
                             bool allow_partial) {
    handlers_.push_back([&signal, type, command, allow_partial](
                            const ZnpCommandType& recvd_type,
                            const ZnpCommand& recvd_command,
                            const std::vector<uint8_t>& data)
                            -> FrameHandlerAction {
      if (recvd_type != type || recvd_command != command) {
        return {false, false};
      }
      typedef std::tuple<std::remove_const_t<std::remove_reference_t<Args>>...>
          ArgTuple;
      ArgTuple arguments;
      try {
        if (allow_partial) {
          arguments = znp::DecodePartial<ArgTuple>(data);
        } else {
          arguments = znp::Decode<ArgTuple>(data);
        }
      } catch (const std::exception& exc) {
        LOG("ZnpApi", warning)
            << "Exception while decoding event: " << exc.what();
        return {false, false};
      }
      polyfill::apply(signal, arguments);
      return {true, false};
    });
  }
};
}  // namespace znp
#endif  //_ZNP_API_H_
