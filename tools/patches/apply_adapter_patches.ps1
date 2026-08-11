# Apply InfoHub safety patches to managed_components/espressif__esp_lvgl_adapter.
#
# esp_lvgl_adapter 0.6.0 includes the upstream bounded TE-vsync wait and
# buffer-switch synchronization updates, but the LVGL v9 GPIO-TE SPI flush still
# has an unbounded TX-done wait after esp_lcd_panel_draw_bitmap().
# If the panel IO completion notification is missed, the LVGL worker keeps the
# adapter lock forever and every esp_lv_adapter_lock() caller times out.
#
# Usage after `idf.py fullclean` / dependency refresh:
#   powershell -ExecutionPolicy Bypass -File tools/patches/apply_adapter_patches.ps1

param(
    [string]$AdapterRoot = "managed_components/espressif__esp_lvgl_adapter"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $AdapterRoot)) {
    Write-Host "[adapter-patch] adapter not present at $AdapterRoot -- skipping (run idf.py reconfigure first)."
    exit 0
}

function Apply-Patch {
    param(
        [string]$Path,
        [string]$Marker,
        [string]$Find,
        [string]$Replace,
        [string]$Label
    )
    if (-not (Test-Path $Path)) {
        Write-Warning ("[adapter-patch] missing file: {0} -- adapter version mismatch?" -f $Path)
        exit 1
    }
    $text = [IO.File]::ReadAllText($Path)
    $textLF = $text -replace "`r`n", "`n"
    $findLF = $Find -replace "`r`n", "`n"
    $replaceLF = $Replace -replace "`r`n", "`n"
    if ($textLF.Contains($Marker)) {
        Write-Host ("[adapter-patch] {0}: already applied." -f $Label)
        return
    }
    if (-not $textLF.Contains($findLF)) {
        Write-Warning ("[adapter-patch] {0}: upstream source does not match expected pattern." -f $Label)
        exit 1
    }
    $patched = $textLF.Replace($findLF, $replaceLF)
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Resolve-Path $Path), $patched, $utf8NoBom)
    Write-Host ("[adapter-patch] {0}: applied." -f $Label)
}

$bridgePath = Join-Path $AdapterRoot 'src/display/bridge/v9/lvgl_bridge_v9.c'
$helperMarker = 'InfoHub local patch: %s TX-done notify timed out'
$helperFind = @'
    return row_bytes;
}

/**
 * @brief Default flush (single buffer, no tear protection)
 */
'@
$helperReplace = @'
    return row_bytes;
}

static bool display_bridge_v9_wait_tx_done(esp_lv_adapter_display_bridge_v9_t *impl,
                                           const char *context)
{
    const TickType_t timeout = pdMS_TO_TICKS(200);
    if (ulTaskNotifyTake(pdTRUE, timeout) != 0) {
        return true;
    }

    ESP_LOGW(TAG, "InfoHub local patch: %s TX-done notify timed out (%ums) -- releasing LVGL flush",
             context ? context : "display", (unsigned)pdTICKS_TO_MS(timeout));
    if (impl && impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }
    return false;
}

/**
 * @brief Default flush (single buffer, no tear protection)
 */
'@
Apply-Patch -Path $bridgePath -Marker $helperMarker -Find $helperFind -Replace $helperReplace -Label 'lvgl_bridge_v9.c helper'

$gpioFind = @'
    /* Wait for transmission to complete */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }

    display_manager_flush_ready(disp);
'@
$gpioReplace = @'
    /* Wait for transmission to complete, but do not deadlock the LVGL worker if
     * the panel IO completion notify is lost under TE/DMA load. */
    const bool tx_done = display_bridge_v9_wait_tx_done(impl, "GPIO TE flush");
    if (tx_done && impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }

    display_manager_flush_ready(disp);
'@
Apply-Patch -Path $bridgePath -Marker 'display_bridge_v9_wait_tx_done(impl, "GPIO TE flush")' -Find $gpioFind -Replace $gpioReplace -Label 'lvgl_bridge_v9.c GPIO TE wait'

Write-Host "[adapter-patch] done."
