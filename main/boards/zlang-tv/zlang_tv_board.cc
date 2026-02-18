#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "i2c_device.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "button.h"
#include "power_save_timer.h"
#include "settings.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <wifi_station.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>

#define TAG "ZlangTVBoard"


LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class ZlangTVBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    bool press_to_talk_enabled_ = false;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 60);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("sleepy");
            
            auto codec = GetAudioCodec();
            codec->EnableInput(false);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto codec = GetAudioCodec();
            codec->EnableInput(true);
            
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_14_1, &font_awesome_14_1});
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            if (!press_to_talk_enabled_) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            power_save_timer_->WakeUp();
            if (press_to_talk_enabled_) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_enabled_) {
                Application::GetInstance().StopListening();
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        Settings settings("vendor");
        press_to_talk_enabled_ = settings.GetInt("press_to_talk", 0) != 0;

        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("PressToTalk"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

    void InitializeGPIO()
    {
        // Initialize GPIOs
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << DISPLAY_RESET_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        gpio_set_level(DISPLAY_RESET_PIN, 1); // 高电平
        
        io_conf.pin_bit_mask = (1ULL << DISPLAY_EN_PIN);
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        gpio_set_level(DISPLAY_EN_PIN, 1); // 高电平
    }

public:
    ZlangTVBoard() : boot_button_(BOOT_BUTTON_GPIO) {  

        InitializeGPIO();
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeIot();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    void SetPressToTalkEnabled(bool enabled) {
        press_to_talk_enabled_ = enabled;

        Settings settings("vendor", true);
        settings.SetInt("press_to_talk", enabled ? 1 : 0);
        ESP_LOGI(TAG, "Press to talk enabled: %d", enabled);
    }

    bool IsPressToTalkEnabled() {
        return press_to_talk_enabled_;
    }
};

DECLARE_BOARD(ZlangTVBoard);


namespace iot {

class PressToTalk : public Thing {
public:
    PressToTalk() : Thing("PressToTalk", "控制对话模式，一种是长按对话，一种是单击后连续对话。") {
        // 定义设备的属性
        properties_.AddBooleanProperty("enabled", "true 表示长按说话模式，false 表示单击说话模式", []() -> bool {
            auto board = static_cast<ZlangTVBoard*>(&Board::GetInstance());
            return board->IsPressToTalkEnabled();
        });

        // 定义设备可以被远程执行的指令
        methods_.AddMethod("SetEnabled", "启用或禁用长按说话模式，调用前需要经过用户确认", ParameterList({
            Parameter("enabled", "true 表示长按说话模式，false 表示单击说话模式", kValueTypeBoolean, true)
        }), [](const ParameterList& parameters) {
            bool enabled = parameters["enabled"].boolean();
            auto board = static_cast<ZlangTVBoard*>(&Board::GetInstance());
            board->SetPressToTalkEnabled(enabled);
        });
    }
};

} // namespace iot

DECLARE_THING(PressToTalk);
