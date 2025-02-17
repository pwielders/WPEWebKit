WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VIDEO PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUDIO PUBLIC ON)

WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_SOURCE PUBLIC ON)

WEBKIT_OPTION_DEFINE(USE_GSTREAMER_GL "Whether to enable support for GStreamer GL" PRIVATE ON)
WEBKIT_OPTION_DEFINE(USE_GSTREAMER_MPEGTS "Whether to enable support for MPEG-TS" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_WPE_VIDEO_PLANE_DISPLAY_DMABUF "Whether to enable support for client-side video rendering" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_WESTEROS_SINK "Westeros-Sink to be used as video-sink for GStreamer video player" PUBLIC OFF)
WEBKIT_OPTION_DEFINE(USE_GSTREAMER_NATIVE_VIDEO "Toggle native video support in GStreamer media player" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_GSTREAMER_NATIVE_AUDIO "Toggle native audio support in GStreamer media player" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_GSTREAMER_TEXT_SINK "Toggle text sink support in GStreamer media player" PRIVATE ON)
