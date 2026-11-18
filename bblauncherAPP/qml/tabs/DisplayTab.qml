import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Launcher 1.0

ScrollPage {
    id: page

    component FieldLabel: Text {
        color: Theme.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsSmall * 1.05
        font.weight: Font.Medium
    }

    component MicroHeading: Text {
        color: Theme.gold
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsMicro
        font.weight: Font.DemiBold
        font.letterSpacing: 1.3
    }

    component SaveFooter: RowLayout {
        spacing: 8
        Item { Layout.fillWidth: true }
        AppButton {
            text: "Save to bbport.ini"
            onClicked: backend.saveBbportIni()
        }
    }

    SectionTitle {
        eyebrow: "Graphics"
        title: "Display & Graphics"
        subtitle: "Resolution, frame pacing, upscaling and post effects. Changes write straight to bbport.ini."
    }

    // ---------------------------------------------------------------- render
    Card {
        Layout.fillWidth: true
        title: "Display & Render Settings"

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 10
            rowSpacing: 6

            FieldLabel { text: "Target Framerate:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_fps }
            FieldLabel { text: "Render Resolution:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_res }

            FieldLabel { text: "Window Mode:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_win_mode }
            FieldLabel { text: "Anisotropic Filtering:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_aniso }

            FieldLabel { text: "Anti-Aliasing:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_aa }
            FieldLabel { text: "Draw Prep Workers:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_prep_workers }

            FieldLabel { text: "GPU Adapter:" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                AppCombo { Layout.fillWidth: true; shim: cmb_gpu }
                AppButton {
                    text: "Rescan"
                    toolTipText: "Re-detect connected GPUs"
                    onClicked: backend.rescanGpus()
                }
            }
            FieldLabel { text: "Live Resolution:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_live_res }

            FieldLabel { text: "Async Pipelines:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_async_pipelines }
            FieldLabel { text: "Game Text Language:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_lang }

            FieldLabel { text: "Texture Quality:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_texture_quality }
        }

        AppCheck {
            Layout.topMargin: 2
            shim: chk_fps_patch
            label: chk_fps_patch.uiText
        }

        SaveFooter { Layout.fillWidth: true }
    }

    // --------------------------------------------------------------- upscaler
    Card {
        Layout.fillWidth: true
        title: "Upscaler Settings (FSR, DLSS & XeSS)"

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 10
            rowSpacing: 6

            FieldLabel { text: "Upscaler Technology:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_upscaler }
            FieldLabel { text: "Upscaler Preset:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_preset }
        }

        AppSlider {
            Layout.fillWidth: true
            visible: grp_custom_scale.visible
            shim: sl_render_scale
            caption: "Render Scale (Custom):"
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            AppCheck { shim: chk_sharpen; label: chk_sharpen.uiText }
            AppCheck { shim: chk_jitter; label: chk_jitter.uiText }
            AppCheck { shim: chk_reactive; label: chk_reactive.uiText }
            AppCheck { shim: chk_object_motion; label: chk_object_motion.uiText }
            AppCheck { shim: chk_fsr4_auto_exposure; label: chk_fsr4_auto_exposure.uiText }
            AppCheck { shim: chk_fsr4_invert_jitter; label: chk_fsr4_invert_jitter.uiText }
        }

        AppSlider { Layout.fillWidth: true; shim: sl_sharpness; caption: "Sharpness:" }
        AppSlider { Layout.fillWidth: true; shim: sl_mip_bias; caption: "Texture Mip Bias:"; signed: true }
        AppSlider { Layout.fillWidth: true; shim: sl_reactive_scale; caption: "Reactive Scale:" }
        AppSlider { Layout.fillWidth: true; shim: sl_reactive_threshold; caption: "Reactive Threshold:" }
        AppSlider { Layout.fillWidth: true; shim: sl_reactive_max; caption: "Reactive Max:" }

        // ---- DLSS ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 8
            visible: grp_dlss.visible

            MicroHeading { text: "DLSS OPTIONS" }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                FieldLabel {
                    Layout.alignment: Qt.AlignVCenter
                    text: "DLSS Model Preset:"
                }
                AppCombo { Layout.fillWidth: true; shim: cmb_dlss_preset }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 24
                rowSpacing: 6

                AppCheck { shim: chk_dlss_auto_exposure; label: chk_dlss_auto_exposure.uiText }
                AppCheck { shim: chk_dlss_mip_bias; label: chk_dlss_mip_bias.uiText }
                AppCheck { shim: chk_dlss_jitter_fade; label: chk_dlss_jitter_fade.uiText }
                AppCheck { shim: chk_upscale_before_post; label: chk_upscale_before_post.uiText }
            }
        }

        // ---- FSR 3.1 ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 8
            visible: grp_fsr3.visible

            MicroHeading { text: "FSR 3.1 TUNING" }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                FieldLabel {
                    Layout.alignment: Qt.AlignVCenter
                    text: "Tuning Preset:"
                }
                AppCombo { Layout.fillWidth: true; shim: cmb_fsr3_preset }
            }

            AppSlider {
                Layout.fillWidth: true
                shim: sl_fsr3_velocity_factor
                caption: "Velocity Factor:"
                scale: 1000
                decimals: 3
            }
            AppSlider {
                Layout.fillWidth: true
                shim: sl_fsr3_reactiveness_scale
                caption: "Reactiveness:"
                scale: 1000
                decimals: 3
            }
            AppSlider {
                Layout.fillWidth: true
                shim: sl_fsr3_shading_change_scale
                caption: "Shading Change:"
                scale: 1000
                decimals: 3
            }
            AppSlider {
                Layout.fillWidth: true
                shim: sl_fsr3_accumulation
                caption: "Accumulation per Frame:"
                scale: 1000
                decimals: 3
            }
            AppSlider {
                Layout.fillWidth: true
                shim: sl_fsr3_min_disocclusion
                caption: "Disocclusion Accumulation:"
                scale: 1000
                decimals: 3
                signed: true
            }
        }

        SaveFooter { Layout.fillWidth: true }
    }

    // ------------------------------------------------------------------- HUD
    Card {
        Layout.fillWidth: true
        title: "HUD & Diagnostics"

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            AppCheck { shim: chk_show_fps; label: chk_show_fps.uiText }
            AppCheck { shim: chk_hud; label: chk_hud.uiText }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 10
            rowSpacing: 6

            FieldLabel { text: "HUD Quadrant:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_hud_quadrant }
            FieldLabel { text: "Debug View:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_debug_view }
        }

        AppSlider { Layout.fillWidth: true; shim: sl_hud_opacity; caption: "HUD Opacity:" }
        AppSlider { Layout.fillWidth: true; shim: sl_hud_scale; caption: "HUD Scale:" }

        SaveFooter { Layout.fillWidth: true }
    }

    // --------------------------------------------------------------- effects
    Card {
        Layout.fillWidth: true
        title: "Effects & Model Detail"
        hint: "Unchecked boxes apply the matching disable patch at the next launch."

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            Repeater {
                model: effectShims
                delegate: AppCheck {
                    required property var modelData
                    shim: modelData
                    label: modelData.uiText
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            FieldLabel {
                Layout.alignment: Qt.AlignVCenter
                text: "Model LOD:"
            }
            AppCombo {
                Layout.fillWidth: true
                Layout.maximumWidth: 320
                shim: cmb_model_lod
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            text: "Game's own anti-aliasing is controlled by the Anti-Aliasing option above."
            color: Theme.textDim
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsMicro
            wrapMode: Text.WordWrap
        }

        SaveFooter { Layout.fillWidth: true }
    }
}
