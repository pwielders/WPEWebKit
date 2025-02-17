/*
 * Copyright (C) 2014, 2015 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2016 Metrological Group B.V.
 * Copyright (C) 2016 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "PlaybackPipeline.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(MEDIA_SOURCE)

#include "AudioTrackPrivateGStreamer.h"
#include "GStreamerCommon.h"
#include "MediaSampleGStreamer.h"
#include "MediaSample.h"
#include "SourceBufferPrivateGStreamer.h"
#include "VideoTrackPrivateGStreamer.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <wtf/MainThread.h>
#include <wtf/RefCounted.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/AtomString.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_mse_debug);
#define GST_CAT_DEFAULT webkit_mse_debug

static Stream* getStreamByTrackId(WebKitMediaSrc*, AtomString);
static Stream* getStreamBySourceBufferPrivate(WebKitMediaSrc*, WebCore::SourceBufferPrivateGStreamer*);

static Stream* getStreamByTrackId(WebKitMediaSrc* source, AtomString trackIdString)
{
    // WebKitMediaSrc should be locked at this point.
    for (Stream* stream : source->priv->streams) {
        if (stream->type != WebCore::Invalid
            && ((stream->audioTrack && stream->audioTrack->id() == trackIdString)
                || (stream->videoTrack && stream->videoTrack->id() == trackIdString) ) ) {
            return stream;
        }
    }
    return nullptr;
}

static Stream* getStreamBySourceBufferPrivate(WebKitMediaSrc* source, WebCore::SourceBufferPrivateGStreamer* sourceBufferPrivate)
{
    for (Stream* stream : source->priv->streams) {
        if (stream->sourceBuffer == sourceBufferPrivate)
            return stream;
    }
    return nullptr;
}

// FIXME: Use gst_app_src_push_sample() instead when we switch to the appropriate GStreamer version.
static GstFlowReturn pushSample(GstAppSrc* appsrc, GstSample* sample)
{
    g_return_val_if_fail(GST_IS_SAMPLE(sample), GST_FLOW_ERROR);

    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps)
        gst_app_src_set_caps(appsrc, caps);
    else
        GST_WARNING_OBJECT(appsrc, "received sample without caps");

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (UNLIKELY(!buffer)) {
        GST_WARNING_OBJECT(appsrc, "received sample without buffer");
        return GST_FLOW_OK;
    }

    // gst_app_src_push_buffer() steals the reference, we need an additional one.
    return gst_app_src_push_buffer(appsrc, gst_buffer_ref(buffer));
}

namespace WebCore {

void PlaybackPipeline::setWebKitMediaSrc(WebKitMediaSrc* webKitMediaSrc)
{
    GST_DEBUG("webKitMediaSrc=%p", webKitMediaSrc);
    m_webKitMediaSrc = webKitMediaSrc;
}

WebKitMediaSrc* PlaybackPipeline::webKitMediaSrc()
{
    return m_webKitMediaSrc.get();
}

MediaSourcePrivate::AddStatus PlaybackPipeline::addSourceBuffer(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;

    if (priv->allTracksConfigured) {
        GST_ERROR_OBJECT(m_webKitMediaSrc.get(), "Adding new source buffers after first data not supported yet");
        return MediaSourcePrivate::NotSupported;
    }

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "State %d", int(GST_STATE(m_webKitMediaSrc.get())));

    Stream* stream = new Stream{ };
    stream->parent = m_webKitMediaSrc.get();

    // Ensure ownership is not transfered to the bin. The appsrc element is managed by its parent Stream.
    stream->appsrc = GST_ELEMENT_CAST(gst_object_ref_sink(gst_element_factory_make("appsrc", nullptr)));

    stream->appsrcNeedDataFlag = false;
    stream->sourceBuffer = sourceBufferPrivate.get();

    // No track has been attached yet.
    stream->type = Invalid;
    stream->caps = nullptr;
    stream->audioTrack = nullptr;
    stream->videoTrack = nullptr;
    stream->presentationSize = WebCore::FloatSize();
    stream->lastEnqueuedTime = MediaTime::invalidTime();

    gst_app_src_set_callbacks(GST_APP_SRC(stream->appsrc), &enabledAppsrcCallbacks, stream->parent, nullptr);
    gst_app_src_set_emit_signals(GST_APP_SRC(stream->appsrc), FALSE);
    gst_app_src_set_stream_type(GST_APP_SRC(stream->appsrc), GST_APP_STREAM_TYPE_SEEKABLE);

    gst_app_src_set_max_bytes(GST_APP_SRC(stream->appsrc), 2 * WTF::MB);
    g_object_set(G_OBJECT(stream->appsrc), "block", FALSE, "min-percent", 20, "format", GST_FORMAT_TIME, nullptr);

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    priv->streams.append(stream);
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    gst_bin_add(GST_BIN_CAST(m_webKitMediaSrc.get()), stream->appsrc);
    gst_element_sync_state_with_parent(stream->appsrc);

    return MediaSourcePrivate::Ok;
}

void PlaybackPipeline::removeSourceBuffer(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    ASSERT(WTF::isMainThread());

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Element removed from MediaSource");
    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;
    Stream* stream = getStreamBySourceBufferPrivate(m_webKitMediaSrc.get(), sourceBufferPrivate.get());
    if (stream)
        priv->streams.removeFirst(stream);
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    if (stream)
        webKitMediaSrcFreeStream(m_webKitMediaSrc.get(), stream);
}

void PlaybackPipeline::attachTrack(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, RefPtr<TrackPrivateBase> trackPrivate, GstCaps* caps)
{
    WebKitMediaSrc* webKitMediaSrc = m_webKitMediaSrc.get();
    int signal = -1;

    GST_OBJECT_LOCK(webKitMediaSrc);
    Stream* stream = getStreamBySourceBufferPrivate(webKitMediaSrc, sourceBufferPrivate.get());
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    ASSERT(stream);
    ASSERT(stream->parent->priv->mediaPlayerPrivate);

    GST_OBJECT_LOCK(webKitMediaSrc);
    unsigned padId = stream->parent->priv->numberOfPads;
    stream->parent->priv->numberOfPads++;
    if (doCapsHaveType(caps, GST_AUDIO_CAPS_TYPE_PREFIX)) {
        stream->type = Audio;
        stream->parent->priv->numberOfAudioStreams++;
        signal = SIGNAL_AUDIO_CHANGED;
        stream->audioTrack = RefPtr<WebCore::AudioTrackPrivateGStreamer>(static_cast<WebCore::AudioTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (doCapsHaveType(caps, GST_VIDEO_CAPS_TYPE_PREFIX)) {
        stream->type = Video;
        stream->parent->priv->numberOfVideoStreams++;
        signal = SIGNAL_VIDEO_CHANGED;
        stream->videoTrack = RefPtr<WebCore::VideoTrackPrivateGStreamer>(static_cast<WebCore::VideoTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (doCapsHaveType(caps, GST_TEXT_CAPS_TYPE_PREFIX)) {
        stream->type = Text;
        stream->parent->priv->numberOfTextStreams++;
        signal = SIGNAL_TEXT_CHANGED;

        // FIXME: Support text tracks.
    } else {
        stream->type = Unknown;
    }
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    const char* mediaType = capsMediaType(caps);
    GST_DEBUG_OBJECT(webKitMediaSrc, "Configured track %s: appsrc=%s, padId=%u, mediaType=%s", trackPrivate->id().string().utf8().data(), GST_ELEMENT_NAME(stream->appsrc), padId, mediaType);

    GRefPtr<GstPad> sourcePad = adoptGRef(gst_element_get_static_pad(stream->appsrc, "src"));
    ASSERT(sourcePad);

    // FIXME: Is padId the best way to identify the Stream? What about trackId?
    g_object_set_data(G_OBJECT(sourcePad.get()), "padId", GINT_TO_POINTER(padId));
    webKitMediaSrcLinkSourcePad(sourcePad.get(), caps, stream);

    if (signal != -1)
        g_signal_emit(G_OBJECT(stream->parent), webKitMediaSrcSignals[signal], 0, nullptr);
}

void PlaybackPipeline::reattachTrack(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, RefPtr<TrackPrivateBase> trackPrivate, GstCaps* caps)
{
    GST_DEBUG("Re-attaching track");

    // FIXME: Maybe remove this method. Now the caps change is managed by gst_appsrc_push_sample() in enqueueSample()
    // and flushAndEnqueueNonDisplayingSamples().

    WebKitMediaSrc* webKitMediaSrc = m_webKitMediaSrc.get();

    GST_OBJECT_LOCK(webKitMediaSrc);
    Stream* stream = getStreamBySourceBufferPrivate(webKitMediaSrc, sourceBufferPrivate.get());
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    ASSERT(stream && stream->type != Invalid);

    int signal = -1;

    GST_OBJECT_LOCK(webKitMediaSrc);
    if (doCapsHaveType(caps, GST_AUDIO_CAPS_TYPE_PREFIX)) {
        ASSERT(stream->type == Audio);
        signal = SIGNAL_AUDIO_CHANGED;
        stream->audioTrack = RefPtr<WebCore::AudioTrackPrivateGStreamer>(static_cast<WebCore::AudioTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (doCapsHaveType(caps, GST_VIDEO_CAPS_TYPE_PREFIX)) {
        ASSERT(stream->type == Video);
        signal = SIGNAL_VIDEO_CHANGED;
        stream->videoTrack = RefPtr<WebCore::VideoTrackPrivateGStreamer>(static_cast<WebCore::VideoTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (doCapsHaveType(caps, GST_TEXT_CAPS_TYPE_PREFIX)) {
        ASSERT(stream->type == Text);
        signal = SIGNAL_TEXT_CHANGED;

        // FIXME: Support text tracks.
    }
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    if (signal != -1)
        g_signal_emit(G_OBJECT(stream->parent), webKitMediaSrcSignals[signal], 0, nullptr);
}

void PlaybackPipeline::notifyDurationChanged()
{
    gst_element_post_message(GST_ELEMENT(m_webKitMediaSrc.get()), gst_message_new_duration_changed(GST_OBJECT(m_webKitMediaSrc.get())));
    // WebKitMediaSrc will ask MediaPlayerPrivateGStreamerMSE for the new duration later, when somebody asks for it.
}

void PlaybackPipeline::markEndOfStream(MediaSourcePrivate::EndOfStreamStatus)
{
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Have EOS");

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    bool allTracksConfigured = priv->allTracksConfigured;
    if (!allTracksConfigured)
        priv->allTracksConfigured = true;
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    if (!allTracksConfigured) {
        gst_element_no_more_pads(GST_ELEMENT(m_webKitMediaSrc.get()));
        webKitMediaSrcDoAsyncDone(m_webKitMediaSrc.get());
    }

    Vector<GstAppSrc*> appsrcs;

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    for (Stream* stream : priv->streams) {
        if (stream->appsrc)
            appsrcs.append(GST_APP_SRC(stream->appsrc));
    }
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    for (GstAppSrc* appsrc : appsrcs)
        gst_app_src_end_of_stream(appsrc);
}

GstPadProbeReturn segmentFixerProbe(GstPad*, GstPadProbeInfo* info, gpointer userData)
{
    GstEvent* event = GST_EVENT(info->data);

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_FLUSH) {
        if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
            GST_DEBUG("Segment fixer probe removed on flush start event: %" GST_PTR_FORMAT, event);
            return GST_PAD_PROBE_REMOVE;
        }
        return GST_PAD_PROBE_OK;
    }

    if (GST_EVENT_TYPE(event) != GST_EVENT_SEGMENT)
        return GST_PAD_PROBE_OK;

    GstSegment* newSegment = static_cast<GstSegment*>(userData);
    GstSegment* segment = nullptr;
    gst_event_parse_segment(event, const_cast<const GstSegment**>(&segment));
    ASSERT(segment);
    gst_segment_copy_into(newSegment, segment);

    GST_TRACE("segment fixed in event %" GST_PTR_FORMAT, event);

    return GST_PAD_PROBE_REMOVE;
}

void PlaybackPipeline::flush(AtomString trackId)
{
    ASSERT(WTF::isMainThread());

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    Stream* stream = getStreamByTrackId(m_webKitMediaSrc.get(), trackId);

    if (!stream || stream->lastEnqueuedTime.isInvalid() || !m_webKitMediaSrc->priv->allTracksConfigured) {
        GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());
        return;
    }

    stream->lastEnqueuedTime = MediaTime::invalidTime();
    GstElement* appsrc = stream->appsrc;
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    if (!appsrc)
        return;

    GST_DEBUG_OBJECT(appsrc, "flush: trackId=%s", trackId.string().utf8().data());

    if (GST_STATE(pipeline()) < GST_STATE_PAUSED) {
        // Flushing at early stage may result in playback failure with 'not-linked' reason.
        // So, lets give pipeline some time to complete pre-roll before flushing this track.
        gst_element_get_state(pipeline(), nullptr, nullptr, 100 * GST_MSECOND);
    }

    gint64 position = GST_CLOCK_TIME_NONE;
    GRefPtr<GstQuery> query = adoptGRef(gst_query_new_position(GST_FORMAT_TIME));
    if (gst_element_query(pipeline(), query.get()))
        gst_query_parse_position(query.get(), 0, &position);

    GST_DEBUG_OBJECT(appsrc, "Position: %" GST_TIME_FORMAT, GST_TIME_ARGS(position));

    if (static_cast<guint64>(position) == GST_CLOCK_TIME_NONE) {
        position = toGstUnsigned64Time(m_webKitMediaSrc->priv->mediaPlayerPrivate->currentMediaTime());
        GST_DEBUG_OBJECT(appsrc, "Can't determine position, using cached one: %" GST_TIME_FORMAT, GST_TIME_ARGS(position));
    }

    double rate = 1.0;
    GstFormat format = GST_FORMAT_TIME;
    gint64 start = GST_CLOCK_TIME_NONE;
    gint64 stop = GST_CLOCK_TIME_NONE;

    query = adoptGRef(gst_query_new_segment(GST_FORMAT_TIME));
    if (gst_element_query(pipeline(), query.get()))
        gst_query_parse_segment(query.get(), &rate, &format, &start, &stop);

#if ENABLE(INSTANT_RATE_CHANGE)
    rate = m_webKitMediaSrc->priv->mediaPlayerPrivate->rate();
#endif

    GST_DEBUG_OBJECT(appsrc, "segment: [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT "], rate: %f",
        GST_TIME_ARGS(start), GST_TIME_ARGS(stop), rate);

    if (!gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_start()))
        GST_WARNING("Failed to send flush-start event for trackId=%s", trackId.string().utf8().data());

#if USE(GSTREAMER_HOLEPUNCH)
    GUniquePtr<GstSegment> segmentHolder(gst_segment_new());
    GstSegment* segment = segmentHolder.get();
    gst_segment_init(segment, GST_FORMAT_TIME);
    gst_segment_do_seek(segment, rate, GST_FORMAT_TIME, GST_SEEK_FLAG_NONE, GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_SET, stop, nullptr);
#else
    GRefPtr<GstElement> sink;
    switch(stream->type) {
    case MediaSourceStreamTypeGStreamer::Video:
        sink = m_player->videoSink();
        break;
    case MediaSourceStreamTypeGStreamer::Audio:
        sink = m_player->audioSink();
        break;
    default:
        GST_WARNING("unexpected stream type %u", stream->type);
        break;
    }
    while (GST_IS_BIN(sink.get())) {
        GUniquePtr<GstIterator> iter(gst_bin_iterate_sinks(GST_BIN_CAST(sink.get())));
        GValue returnValue = G_VALUE_INIT;
        auto result = gst_iterator_next(iter.get(), &returnValue);
        ASSERT_UNUSED(result, result == GST_ITERATOR_OK);
        sink = adoptGRef(GST_ELEMENT(g_value_dup_object(&returnValue)));
        g_value_unset(&returnValue);
    }

    ASSERT(GST_IS_BASE_SINK(sink.get()));
    GstSegment* segment = &GST_BASE_SINK_CAST(sink.get())->segment;
#endif

    GRefPtr<GstPad> sinkPad = gst_element_get_static_pad(appsrc, "src");
    GRefPtr<GstPad> srcPad = sinkPad ? gst_pad_get_peer(sinkPad.get()) : nullptr;
    if (srcPad)
        gst_pad_add_probe(srcPad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH), segmentFixerProbe, gst_segment_copy(segment), (GDestroyNotify) gst_segment_free);

    GST_DEBUG_OBJECT(appsrc, "Sending new segment event");

    if (!gst_base_src_new_seamless_segment(GST_BASE_SRC(appsrc), segment->start, segment->stop, segment->time))
        GST_WARNING("Failed to configure seamless segment event for trackId=%s", trackId.string().utf8().data());

    if (!gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_stop(false)))
        GST_WARNING("Failed to send flush-stop event for trackId=%s", trackId.string().utf8().data());

    GST_DEBUG_OBJECT(appsrc, "trackId=%s flushed", trackId.string().utf8().data());
}

void PlaybackPipeline::enqueueSample(Ref<MediaSample>&& mediaSample)
{
    ASSERT(WTF::isMainThread());

    AtomString trackId = mediaSample->trackID();

    GST_TRACE("enqueing sample trackId=%s PTS=%s presentationSize=%.0fx%.0f at %" GST_TIME_FORMAT " duration: %" GST_TIME_FORMAT,
        trackId.string().utf8().data(), mediaSample->presentationTime().toString().ascii().data(),
        mediaSample->presentationSize().width(), mediaSample->presentationSize().height(),
        GST_TIME_ARGS(WebCore::toGstClockTime(mediaSample->presentationTime())),
        GST_TIME_ARGS(WebCore::toGstClockTime(mediaSample->duration())));

    // No need to lock to access the Stream here because the only chance of conflict with this read and with the usage
    // of the sample fields done in this method would be the deletion of the stream. However, that operation can only
    // happen in the main thread, but we're already there. Therefore there's no conflict and locking would only cause
    // a performance penalty on the readers working in other threads.
    Stream* stream = getStreamByTrackId(m_webKitMediaSrc.get(), trackId);

    if (!stream) {
        GST_WARNING("No stream!");
        return;
    }

    if (!stream->sourceBuffer->isReadyForMoreSamples(trackId)) {
        GST_DEBUG("enqueueSample: skip adding new sample for trackId=%s, SB is not ready yet", trackId.string().utf8().data());
        return;
    }

    // This field doesn't change after creation, no need to lock.
    GstElement* appsrc = stream->appsrc;

    // Only modified by the main thread, no need to lock.
    MediaTime lastEnqueuedTime = stream->lastEnqueuedTime;

    ASSERT(mediaSample->platformSample().type == PlatformSample::GStreamerSampleType);
    GRefPtr<GstSample> gstSample = mediaSample->platformSample().sample.gstSample;
    if (gstSample && gst_sample_get_buffer(gstSample.get())) {
        GstBuffer* buffer = gst_sample_get_buffer(gstSample.get());
        lastEnqueuedTime = mediaSample->presentationTime();

        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DECODE_ONLY);
        pushSample(GST_APP_SRC(appsrc), gstSample.get());
        // gst_app_src_push_sample() uses transfer-none for gstSample.

        stream->lastEnqueuedTime = lastEnqueuedTime;
    }
}

void PlaybackPipeline::allSamplesInTrackEnqueued(const AtomString& trackId)
{
    Stream* stream = getStreamByTrackId(m_webKitMediaSrc.get(), trackId);
    gst_app_src_end_of_stream(GST_APP_SRC(stream->appsrc));
}

GstElement* PlaybackPipeline::pipeline()
{
    if (!m_webKitMediaSrc || !GST_ELEMENT_PARENT(GST_ELEMENT(m_webKitMediaSrc.get())))
        return nullptr;

    return GST_ELEMENT_PARENT(GST_ELEMENT_PARENT(GST_ELEMENT(m_webKitMediaSrc.get())));
}

} // namespace WebCore.

#endif // USE(GSTREAMER)
