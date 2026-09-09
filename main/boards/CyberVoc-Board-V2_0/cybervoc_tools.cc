#include "cybervoc_tools.h"
#include "CyberVoc.h"
#include "cyber_base_control.h"
#include "audio_analysis.h"
#include "mcp_server.h"
#include "board.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include "customer_ui/feature_ui.h"
#include "car_mode/expression_pack_manager.h"

#define TAG "CyberVocTools"

void CyberVocTools::Initialize(EspS3Cat* board)
{
    auto &mcp_server = McpServer::GetInstance();

    // Cyber base action control
    mcp_server.AddTool("self.cyber_base.set_action", "Cyber base action control. Available actions:\n"
                       "shark_head: 摇头动作\n"
                       "shark_head_decay: 缓慢摇头动作\n"
                       "look_around: 环顾四周动作\n"
                       "cat_nuzzle: 蹭头撒娇动作\n"
                       "calibrate: 校准底座\n"
                       "turn_right: 向右转动作\n"
                       "turn_left: 向左转动作\n"
                       "cat_go_home: 归位回正动作\n",
    PropertyList({
        Property("action", kPropertyTypeString),
    }), [board](const PropertyList & properties) -> ReturnValue {
        const std::string &action = properties["action"].value<std::string>();
        int action_value = -1;

        ESP_LOGI(TAG, "&&& Do Action: %s", action.c_str());
        if (action == "shark_head") {
            action_value = CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD;
        } else if (action == "shark_head_decay") {
            action_value = CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD_DECAY;
        } else if (action == "look_around")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_LOOK_AROUND;
        } else if (action == "cat_nuzzle")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_NUZZLE;
        }  else if (action == "turn_right")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_TO_RIGHT;
        } else if (action == "turn_left")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_TO_LEFT;
        } else if (action == "cat_go_home")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_GO_HOME;
        } else if (action == "calibrate")
        {
            cyber_base_control_set_calibrate();
            BaseControl* base_control = board->GetBaseControl();
            if (base_control != nullptr) {
                bool completed = base_control->WaitForCalibrationComplete(30000);
                if (!completed) {
                    ESP_LOGW(TAG, "Calibration wait timeout");
                    return false;
                }
            }
        } else
        {
            return false;
        }

        if (action_value != -1)
        {
            esp_err_t ret = cyber_base_control_set_action(action_value);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set action: %d", ret);
                return false;
            }
        }
        return true;
    });

    // Audio analysis mode control
    mcp_server.AddTool("self.cyber_base.set_audio_mode", "Set audio analysis mode. Available modes:\n"
                       "doa_follow: DOA 声音方向跟随模式\n"
                       "disabled: 禁用音频分析",
    PropertyList({
        Property("mode", kPropertyTypeString),
    }), [board](const PropertyList & properties) -> ReturnValue {
        const std::string &mode = properties["mode"].value<std::string>();
        AudioAnalysisMode analysis_mode = AudioAnalysisMode::DISABLED;

        if (mode == "doa_follow") {
            analysis_mode = AudioAnalysisMode::DOA_FOLLOW;
        } else if (mode == "disabled")
        {
            analysis_mode = AudioAnalysisMode::DISABLED;
        } else
        {
            ESP_LOGE(TAG, "Unknown audio analysis mode: %s", mode.c_str());
            return false;
        }
    
        board->SetAudioAnalysisMode(analysis_mode);
        ESP_LOGI(TAG, "Audio analysis mode set to: %s", mode.c_str());
        return true;
    });

    mcp_server.AddTool("self.timer.start", "启动通用倒计时器（1-1440 分钟）。计时离开页面后继续运行。",
    PropertyList({
        Property("minutes", kPropertyTypeInteger, 5, 1, 1440),
    }), [](const PropertyList& properties) -> ReturnValue {
        int minutes = properties["minutes"].value<int>();
        return feature_timer_start_minutes(minutes);
    });

    mcp_server.AddTool("self.timer.control", "控制通用倒计时器：pause、resume 或 cancel。",
    PropertyList({
        Property("action", kPropertyTypeString),
    }), [](const PropertyList& properties) -> ReturnValue {
        const std::string &action = properties["action"].value<std::string>();
        if (action == "pause") return feature_timer_pause();
        if (action == "resume") return feature_timer_resume();
        if (action == "cancel") return feature_timer_cancel();
        ESP_LOGE(TAG, "Unknown timer action: %s", action.c_str());
        return false;
    });

    mcp_server.AddTool("self.timer.status", "查询通用倒计时器状态和剩余秒数。",
    PropertyList(std::vector<Property>{}), [](const PropertyList&) -> ReturnValue {
        const char* state = "idle";
        switch (feature_timer_get_state()) {
            case FEATURE_TIMER_RUNNING: state = "running"; break;
            case FEATURE_TIMER_PAUSED: state = "paused"; break;
            case FEATURE_TIMER_FINISHED: state = "finished"; break;
            default: break;
        }
        return std::string("state=") + state +
               ", remaining_seconds=" + std::to_string(feature_timer_get_remaining_seconds());
    });

    mcp_server.AddTool("self.car_mode.set_enabled", "开启或关闭车载显示模式。该模式仅改变屏幕，不驱动底座。",
    PropertyList({Property("enabled", kPropertyTypeBoolean, false)}),
    [](const PropertyList& properties) -> ReturnValue {
        return feature_car_set_enabled(properties["enabled"].value<bool>());
    });

    mcp_server.AddTool("self.car_mode.recalibrate", "重新开始车载模式 IMU 静止校准。设备需固定静止至少两秒。",
    PropertyList(std::vector<Property>{}), [](const PropertyList&) -> ReturnValue {
        if (!feature_car_is_enabled()) return false;
        feature_car_recalibrate();
        return true;
    });

    mcp_server.AddTool("self.car_mode.status", "查询车载模式启用、校准和当前运动事件状态。",
    PropertyList(std::vector<Property>{}), [](const PropertyList&) -> ReturnValue {
        return std::string("enabled=") + (feature_car_is_enabled() ? "true" : "false") +
               ", calibrated=" + (feature_car_is_calibrated() ? "true" : "false") +
               ", event=" + feature_car_get_event_name();
    });

    mcp_server.AddTool("self.expression_packs.rescan", "重新扫描 TF 卡 /miaoban/packs 下的 MEPACK 表情包。",
    PropertyList(std::vector<Property>{}), [](const PropertyList&) -> ReturnValue {
        expression_packs::RequestScan();
        return true;
    });

    mcp_server.AddTool("self.expression_packs.select", "选择内置或 TF 卡表情包；TF 包必须先扫描成功。",
    PropertyList({Property("source", kPropertyTypeString),
                  Property("pack_id", kPropertyTypeString),
                  Property("version", kPropertyTypeInteger, 1, 1, 1000000)}),
    [](const PropertyList& properties) -> ReturnValue {
        const std::string& source = properties["source"].value<std::string>();
        if (source != "builtin" && source != "tf") return false;
        return expression_packs::Select(source == "tf" ? expression_packs::Source::Tf :
                                                         expression_packs::Source::Builtin,
                                        properties["pack_id"].value<std::string>().c_str(),
                                        properties["version"].value<int>());
    });

    mcp_server.AddTool("self.expression_packs.status", "查询当前表情包和最近一次 TF 扫描/加载状态。",
    PropertyList(std::vector<Property>{}), [](const PropertyList&) -> ReturnValue {
        char current[96]{}, status[112]{};
        expression_packs::CurrentLabel(current, sizeof(current));
        expression_packs::StatusText(status, sizeof(status));
        return std::string("current=") + current + ", status=" + status;
    });
}
