-- Bypass the epaper-image-convert dithering pipeline and serve a truecolour
-- PNG instead. For panels that render more than 6 colours (an RGB565 TFT or
-- LCD) the converter is a downgrade rather than a feature: its only outputs
-- are the 6-colour ACeP palette and a 16-step grey ramp.
--
-- Reported by the device as "full_colour": true in GET /api/system-info, and
-- overridable per device in the web UI. Defaults to off so no existing
-- e-paper frame changes behaviour.
ALTER TABLE devices ADD COLUMN full_colour BOOLEAN NOT NULL DEFAULT FALSE;
