// Copyright 2020-2021 ros2_control Development Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "hardware_interface/actuator.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hardware_interface/actuator_interface.hpp"
#include "hardware_interface/handle.hpp"
#include "hardware_interface/hardware_info.hpp"
#include "hardware_interface/lifecycle_helpers.hpp"
#include "hardware_interface/types/hardware_interface_return_values.hpp"
#include "hardware_interface/types/lifecycle_state_names.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "rclcpp/logging.hpp"
#include "rclcpp_lifecycle/node_interfaces/lifecycle_node_interface.hpp"
#include "rclcpp_lifecycle/state.hpp"

// TODO(destogl): Add transition names also
namespace hardware_interface
{
using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

Actuator::Actuator(std::unique_ptr<ActuatorInterface> impl) : impl_(std::move(impl)) {}

Actuator::Actuator(Actuator && other) noexcept
{
  std::lock_guard<std::recursive_mutex> lock(other.actuators_mutex_);
  impl_ = std::move(other.impl_);
  last_read_cycle_time_ = rclcpp::Time(0, 0, RCL_CLOCK_UNINITIALIZED);
  last_write_cycle_time_ = rclcpp::Time(0, 0, RCL_CLOCK_UNINITIALIZED);
}

const rclcpp_lifecycle::State & Actuator::initialize(
  const HardwareInfo & actuator_info, rclcpp::Logger logger,
  rclcpp::node_interfaces::NodeClockInterface::SharedPtr clock_interface)
{
  // This is done for backward compatibility with the old initialize method.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  return this->initialize(actuator_info, logger, clock_interface->get_clock());
#pragma GCC diagnostic pop
}

const rclcpp_lifecycle::State & Actuator::initialize(
  const HardwareInfo & actuator_info, rclcpp::Logger logger, rclcpp::Clock::SharedPtr clock)
{
  hardware_interface::HardwareComponentParams params;
  params.hardware_info = actuator_info;
  params.logger = logger;
  params.clock = clock;
  return initialize(params);
}

const rclcpp_lifecycle::State & Actuator::initialize(
  const hardware_interface::HardwareComponentParams & params)
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  if (impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_UNKNOWN)
  {
    switch (impl_->init(params))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED,
            lifecycle_state_names::UNCONFIGURED));
        break;
      case CallbackReturn::FAILURE:
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED, lifecycle_state_names::FINALIZED));
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::configure()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  if (impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED)
  {
    switch (impl_->on_configure(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE, lifecycle_state_names::INACTIVE));
        break;
      case CallbackReturn::FAILURE:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED,
            lifecycle_state_names::UNCONFIGURED));
        break;
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(error());
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::cleanup()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  impl_->enable_introspection(false);
  if (impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
  {
    switch (impl_->on_cleanup(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED,
            lifecycle_state_names::UNCONFIGURED));
        break;
      case CallbackReturn::FAILURE:
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(error());
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::shutdown()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  impl_->enable_introspection(false);
  if (
    impl_->get_lifecycle_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_UNKNOWN &&
    impl_->get_lifecycle_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED)
  {
    switch (impl_->on_shutdown(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED, lifecycle_state_names::FINALIZED));
        break;
      case CallbackReturn::FAILURE:
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(error());
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::activate()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  last_read_cycle_time_ = rclcpp::Time(0, 0, RCL_CLOCK_UNINITIALIZED);
  last_write_cycle_time_ = rclcpp::Time(0, 0, RCL_CLOCK_UNINITIALIZED);
  read_statistics_.reset_statistics();
  write_statistics_.reset_statistics();
  if (impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
  {
    impl_->prepare_for_activation();
    switch (impl_->on_activate(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->enable_introspection(true);
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, lifecycle_state_names::ACTIVE));
        break;
      case CallbackReturn::FAILURE:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE, lifecycle_state_names::INACTIVE));
        break;
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(error());
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::deactivate()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  impl_->enable_introspection(false);
  if (impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    switch (impl_->on_deactivate(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE, lifecycle_state_names::INACTIVE));
        break;
      case CallbackReturn::FAILURE:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, lifecycle_state_names::ACTIVE));
        break;
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(error());
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

const rclcpp_lifecycle::State & Actuator::error()
{
  std::unique_lock<std::recursive_mutex> lock(actuators_mutex_);
  impl_->enable_introspection(false);
  if (
    impl_->get_lifecycle_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_UNKNOWN &&
    impl_->get_lifecycle_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED)
  {
    switch (impl_->on_error(impl_->get_lifecycle_state()))
    {
      case CallbackReturn::SUCCESS:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED,
            lifecycle_state_names::UNCONFIGURED));
        break;
      case CallbackReturn::FAILURE:
      case CallbackReturn::ERROR:
        impl_->set_lifecycle_state(
          rclcpp_lifecycle::State(
            lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED, lifecycle_state_names::FINALIZED));
        break;
    }
  }
  return impl_->get_lifecycle_state();
}

std::vector<StateInterface::ConstSharedPtr> Actuator::export_state_interfaces()
{
// BEGIN (Handle export change): for backward compatibility, can be removed if
// export_command_interfaces() method is removed
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  std::vector<StateInterface> interfaces = impl_->export_state_interfaces();
#pragma GCC diagnostic pop
  // END: for backward compatibility

  // If no StateInterfaces has been exported, this could mean:
  // a) there is nothing to export -> on_export_state_interfaces() does return nothing as well
  // b) default implementation for export_state_interfaces() is used -> new functionality ->
  // Framework exports and creates everything
  if (interfaces.empty())
  {
    return impl_->on_export_state_interfaces();
  }

  // BEGIN (Handle export change): for backward compatibility, can be removed if
  // export_command_interfaces() method is removed
  std::vector<StateInterface::ConstSharedPtr> interface_ptrs;
  interface_ptrs.reserve(interfaces.size());
  for (auto const & interface : interfaces)
  {
    interface_ptrs.emplace_back(std::make_shared<const StateInterface>(interface));
  }
  return interface_ptrs;
  // END: for backward compatibility
}

std::vector<CommandInterface::SharedPtr> Actuator::export_command_interfaces()
{
// BEGIN (Handle export change): for backward compatibility, can be removed if
// export_command_interfaces() method is removed
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  std::vector<CommandInterface> interfaces = impl_->export_command_interfaces();
#pragma GCC diagnostic pop
  // END: for backward compatibility

  // If no CommandInterface has been exported, this could mean:
  // a) there is nothing to export -> on_export_command_interfaces() does return nothing as well
  // b) default implementation for export_command_interfaces() is used -> new functionality ->
  // Framework exports and creates everything
  if (interfaces.empty())
  {
    return impl_->on_export_command_interfaces();
  }
  // BEGIN (Handle export change): for backward compatibility, can be removed if
  // export_command_interfaces() method is removed
  std::vector<CommandInterface::SharedPtr> interface_ptrs;
  interface_ptrs.reserve(interfaces.size());
  for (auto & interface : interfaces)
  {
    interface_ptrs.emplace_back(std::make_shared<CommandInterface>(std::move(interface)));
  }
  return interface_ptrs;
  // END: for backward compatibility
}

return_type Actuator::prepare_command_mode_switch(
  const std::vector<std::string> & start_interfaces,
  const std::vector<std::string> & stop_interfaces)
{
  return impl_->prepare_command_mode_switch(start_interfaces, stop_interfaces);
}

return_type Actuator::perform_command_mode_switch(
  const std::vector<std::string> & start_interfaces,
  const std::vector<std::string> & stop_interfaces)
{
  return impl_->perform_command_mode_switch(start_interfaces, stop_interfaces);
}

const std::string & Actuator::get_name() const { return impl_->get_name(); }

const std::string & Actuator::get_group_name() const { return impl_->get_group_name(); }

const rclcpp_lifecycle::State & Actuator::get_lifecycle_state() const
{
  return impl_->get_lifecycle_state();
}

const rclcpp::Time & Actuator::get_last_read_time() const { return last_read_cycle_time_; }

const rclcpp::Time & Actuator::get_last_write_time() const { return last_write_cycle_time_; }

const HardwareComponentStatisticsCollector & Actuator::get_read_statistics() const
{
  return read_statistics_;
}

const HardwareComponentStatisticsCollector & Actuator::get_write_statistics() const
{
  return write_statistics_;
}

return_type Actuator::read(const rclcpp::Time & time, const rclcpp::Duration & period)
{
  if (lifecycleStateThatRequiresNoAction(impl_->get_lifecycle_state().id()))
  {
    last_read_cycle_time_ = time;
    return return_type::OK;
  }
  if (
    impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
    impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    const auto trigger_result = impl_->trigger_read(time, period);
    if (trigger_result.result == return_type::ERROR)
    {
      error();
    }
    if (trigger_result.successful)
    {
      if (trigger_result.execution_time.has_value())
      {
        read_statistics_.execution_time->AddMeasurement(
          static_cast<double>(trigger_result.execution_time.value().count()) / 1.e3);
      }
      if (last_read_cycle_time_.get_clock_type() != RCL_CLOCK_UNINITIALIZED)
      {
        read_statistics_.periodicity->AddMeasurement(
          1.0 / (time - last_read_cycle_time_).seconds());
      }
      last_read_cycle_time_ = time;
    }
    return trigger_result.result;
  }
  return return_type::OK;
}

return_type Actuator::write(const rclcpp::Time & time, const rclcpp::Duration & period)
{
  if (lifecycleStateThatRequiresNoAction(impl_->get_lifecycle_state().id()))
  {
    last_write_cycle_time_ = time;
    return return_type::OK;
  }
  if (
    impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
    impl_->get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    const auto trigger_result = impl_->trigger_write(time, period);
    if (trigger_result.result == return_type::ERROR)
    {
      error();
    }
    if (trigger_result.successful)
    {
      if (trigger_result.execution_time.has_value())
      {
        write_statistics_.execution_time->AddMeasurement(
          static_cast<double>(trigger_result.execution_time.value().count()) / 1.e3);
      }
      if (last_write_cycle_time_.get_clock_type() != RCL_CLOCK_UNINITIALIZED)
      {
        write_statistics_.periodicity->AddMeasurement(
          1.0 / (time - last_write_cycle_time_).seconds());
      }
      last_write_cycle_time_ = time;
    }
    return trigger_result.result;
  }
  return return_type::OK;
}

std::recursive_mutex & Actuator::get_mutex() { return actuators_mutex_; }
}  // namespace hardware_interface
