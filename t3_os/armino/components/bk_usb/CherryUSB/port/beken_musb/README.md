# Note

## Support Chip List

- BK7236
- BK7258
- BK7286

## Before Use

Your should implement `usb_dc_low_level_init` and `usb_dc_low_level_deinit`.
- Enable or disable USB clock and set USB clock for 48M.
- Enable or disable gpio and gpio clk for usb dp and dm.
- Enable or disable usb irq
