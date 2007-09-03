/*
 * Farsight Voice+Video library
 *
 *  Copyright 2007 Collabora Ltd, 
 *  Copyright 2007 Nokia Corporation
 *   @author: Philippe Kalaf <philippe.kalaf@collabora.co.uk>.
 *  Copyright 2007 Wim Taymans <wim.taymans@gmail.com>
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

/**
 * SECTION:element-gstrtpjitterbuffer
 * @short_description: buffer, reorder and remove duplicate RTP packets to
 * compensate for network oddities.
 *
 * <refsect2>
 * <para>
 * This element reorders and removes duplicate RTP packets as they are received
 * from a network source. It will also wait for missing packets up to a
 * configurable time limit using the ::latency property. Packets arriving too
 * late are considered to be lost packets.
 * </para>
 * <para>
 * This element acts as a live element and so adds ::latency to the pipeline.
 * </para>
 * <para>
 * The element needs the clock-rate of the RTP payload in order to estimate the
 * delay. This information is obtained either from the caps on the sink pad or,
 * when no caps are present, from the ::request-pt-map signal. To clear the
 * previous pt-map use the ::clear-pt-map signal.
 * </para>
 * <para>
 * This element will automatically be used inside gstrtpbin.
 * </para>
 * <title>Example pipelines</title>
 * <para>
 * <programlisting>
 * gst-launch rtspsrc location=rtsp://192.168.1.133:8554/mpeg1or2AudioVideoTest ! gstrtpjitterbuffer ! rtpmpvdepay ! mpeg2dec ! xvimagesink
 * </programlisting>
 * Connect to a streaming server and decode the MPEG video. The jitterbuffer is
 * inserted into the pipeline to smooth out network jitter and to reorder the
 * out-of-order RTP packets.
 * </para>
 * </refsect2>
 *
 * Last reviewed on 2007-05-28 (0.10.5)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <gst/rtp/gstrtpbuffer.h>

#include "gstrtpbin-marshal.h"

#include "gstrtpjitterbuffer.h"
#include "rtpjitterbuffer.h"

GST_DEBUG_CATEGORY (rtpjitterbuffer_debug);
#define GST_CAT_DEFAULT (rtpjitterbuffer_debug)

/* low and high threshold tell the queue when to start and stop buffering */
#define LOW_THRESHOLD 0.2
#define HIGH_THRESHOLD 0.8

/* elementfactory information */
static const GstElementDetails gst_rtp_jitter_buffer_details =
GST_ELEMENT_DETAILS ("RTP packet jitter-buffer",
    "Filter/Network/RTP",
    "A buffer that deals with network jitter and other transmission faults",
    "Philippe Kalaf <philippe.kalaf@collabora.co.uk>, "
    "Wim Taymans <wim.taymans@gmail.com>");

/* RTPJitterBuffer signals and args */
enum
{
  SIGNAL_REQUEST_PT_MAP,
  SIGNAL_CLEAR_PT_MAP,
  LAST_SIGNAL
};

#define DEFAULT_LATENCY_MS      200
#define DEFAULT_DROP_ON_LATENCY FALSE
#define DEFAULT_TS_OFFSET       0

enum
{
  PROP_0,
  PROP_LATENCY,
  PROP_DROP_ON_LATENCY,
  PROP_TS_OFFSET
};

#define JBUF_LOCK(priv)   (g_mutex_lock ((priv)->jbuf_lock))

#define JBUF_LOCK_CHECK(priv,label) G_STMT_START {    \
  JBUF_LOCK (priv);                                   \
  if (priv->srcresult != GST_FLOW_OK)                 \
    goto label;                                       \
} G_STMT_END

#define JBUF_UNLOCK(priv) (g_mutex_unlock ((priv)->jbuf_lock))
#define JBUF_WAIT(priv)   (g_cond_wait ((priv)->jbuf_cond, (priv)->jbuf_lock))

#define JBUF_WAIT_CHECK(priv,label) G_STMT_START {    \
  JBUF_WAIT(priv);                                    \
  if (priv->srcresult != GST_FLOW_OK)                 \
    goto label;                                       \
} G_STMT_END

#define JBUF_SIGNAL(priv) (g_cond_signal ((priv)->jbuf_cond))

struct _GstRtpJitterBufferPrivate
{
  GstPad *sinkpad, *srcpad;

  RTPJitterBuffer *jbuf;
  GMutex *jbuf_lock;
  GCond *jbuf_cond;

  /* properties */
  guint latency_ms;
  gboolean drop_on_latency;
  gint64 ts_offset;

  /* the last seqnum we pushed out */
  guint32 last_popped_seqnum;
  /* the next expected seqnum */
  guint32 next_seqnum;

  /* state */
  gboolean eos;

  /* clock rate and rtp timestamp offset */
  gint32 clock_rate;
  gint64 clock_base;
  guint64 exttimestamp;
  gint64 prev_ts_offset;

  /* when we are shutting down */
  GstFlowReturn srcresult;
  gboolean blocked;

  /* for sync */
  GstSegment segment;
  GstClockID clock_id;
  guint32 waiting_seqnum;
  /* the latency of the upstream peer, we have to take this into account when
   * synchronizing the buffers. */
  GstClockTime peer_latency;

  /* some accounting */
  guint64 num_late;
  guint64 num_duplicates;
};

#define GST_RTP_JITTER_BUFFER_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), GST_TYPE_RTP_JITTER_BUFFER, \
                                GstRtpJitterBufferPrivate))

static GstStaticPadTemplate gst_rtp_jitter_buffer_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp, "
        "clock-rate = (int) [ 1, 2147483647 ]"
        /* "payload = (int) , "
         * "encoding-name = (string) "
         */ )
    );

static GstStaticPadTemplate gst_rtp_jitter_buffer_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp"
        /* "payload = (int) , "
         * "clock-rate = (int) , "
         * "encoding-name = (string) "
         */ )
    );

static guint gst_rtp_jitter_buffer_signals[LAST_SIGNAL] = { 0 };

GST_BOILERPLATE (GstRtpJitterBuffer, gst_rtp_jitter_buffer, GstElement,
    GST_TYPE_ELEMENT);

/* object overrides */
static void gst_rtp_jitter_buffer_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_rtp_jitter_buffer_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_rtp_jitter_buffer_dispose (GObject * object);

/* element overrides */
static GstStateChangeReturn gst_rtp_jitter_buffer_change_state (GstElement
    * element, GstStateChange transition);

/* pad overrides */
static GstCaps *gst_rtp_jitter_buffer_getcaps (GstPad * pad);

/* sinkpad overrides */
static gboolean gst_jitter_buffer_sink_setcaps (GstPad * pad, GstCaps * caps);
static gboolean gst_rtp_jitter_buffer_sink_event (GstPad * pad,
    GstEvent * event);
static GstFlowReturn gst_rtp_jitter_buffer_chain (GstPad * pad,
    GstBuffer * buffer);

/* srcpad overrides */
static gboolean
gst_rtp_jitter_buffer_src_activate_push (GstPad * pad, gboolean active);
static void gst_rtp_jitter_buffer_loop (GstRtpJitterBuffer * jitterbuffer);
static gboolean gst_rtp_jitter_buffer_query (GstPad * pad, GstQuery * query);

static void
gst_rtp_jitter_buffer_clear_pt_map (GstRtpJitterBuffer * jitterbuffer);

static void
gst_rtp_jitter_buffer_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rtp_jitter_buffer_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rtp_jitter_buffer_sink_template));
  gst_element_class_set_details (element_class, &gst_rtp_jitter_buffer_details);
}

static void
gst_rtp_jitter_buffer_class_init (GstRtpJitterBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  g_type_class_add_private (klass, sizeof (GstRtpJitterBufferPrivate));

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_dispose);

  gobject_class->set_property = gst_rtp_jitter_buffer_set_property;
  gobject_class->get_property = gst_rtp_jitter_buffer_get_property;

  /**
   * GstRtpJitterBuffer::latency:
   * 
   * The maximum latency of the jitterbuffer. Packets will be kept in the buffer
   * for at most this time.
   */
  g_object_class_install_property (gobject_class, PROP_LATENCY,
      g_param_spec_uint ("latency", "Buffer latency in ms",
          "Amount of ms to buffer", 0, G_MAXUINT, DEFAULT_LATENCY_MS,
          G_PARAM_READWRITE));
  /**
   * GstRtpJitterBuffer::drop-on-latency:
   * 
   * Drop oldest buffers when the queue is completely filled. 
   */
  g_object_class_install_property (gobject_class, PROP_DROP_ON_LATENCY,
      g_param_spec_boolean ("drop-on-latency",
          "Drop buffers when maximum latency is reached",
          "Tells the jitterbuffer to never exceed the given latency in size",
          DEFAULT_DROP_ON_LATENCY, G_PARAM_READWRITE));
  /**
   * GstRtpJitterBuffer::ts-offset:
   * 
   * Adjust RTP timestamps in the jitterbuffer with offset.
   */
  g_object_class_install_property (gobject_class, PROP_TS_OFFSET,
      g_param_spec_int64 ("ts-offset",
          "Timestamp Offset",
          "Adjust buffer RTP timestamps with offset in nanoseconds", G_MININT64,
          G_MAXINT64, DEFAULT_TS_OFFSET, G_PARAM_READWRITE));
  /**
   * GstRtpJitterBuffer::request-pt-map:
   * @buffer: the object which received the signal
   * @pt: the pt
   *
   * Request the payload type as #GstCaps for @pt.
   */
  gst_rtp_jitter_buffer_signals[SIGNAL_REQUEST_PT_MAP] =
      g_signal_new ("request-pt-map", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRtpJitterBufferClass,
          request_pt_map), NULL, NULL, gst_rtp_bin_marshal_BOXED__UINT,
      GST_TYPE_CAPS, 1, G_TYPE_UINT);
  /**
   * GstRtpJitterBuffer::clear-pt-map:
   * @buffer: the object which received the signal
   *
   * Invalidate the clock-rate as obtained with the ::request-pt-map signal.
   */
  gst_rtp_jitter_buffer_signals[SIGNAL_CLEAR_PT_MAP] =
      g_signal_new ("clear-pt-map", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRtpJitterBufferClass,
          clear_pt_map), NULL, NULL, g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE, 0, G_TYPE_NONE);

  gstelement_class->change_state = gst_rtp_jitter_buffer_change_state;

  klass->clear_pt_map = GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_clear_pt_map);

  GST_DEBUG_CATEGORY_INIT
      (rtpjitterbuffer_debug, "rtpjitterbuffer", 0, "RTP Jitter Buffer");
}

static void
gst_rtp_jitter_buffer_init (GstRtpJitterBuffer * jitterbuffer,
    GstRtpJitterBufferClass * klass)
{
  GstRtpJitterBufferPrivate *priv;

  priv = GST_RTP_JITTER_BUFFER_GET_PRIVATE (jitterbuffer);
  jitterbuffer->priv = priv;

  priv->latency_ms = DEFAULT_LATENCY_MS;
  priv->drop_on_latency = DEFAULT_DROP_ON_LATENCY;

  priv->jbuf = rtp_jitter_buffer_new ();
  priv->jbuf_lock = g_mutex_new ();
  priv->jbuf_cond = g_cond_new ();

  priv->waiting_seqnum = -1;

  priv->srcpad =
      gst_pad_new_from_static_template (&gst_rtp_jitter_buffer_src_template,
      "src");

  gst_pad_set_activatepush_function (priv->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_src_activate_push));
  gst_pad_set_query_function (priv->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_query));
  gst_pad_set_getcaps_function (priv->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_getcaps));

  priv->sinkpad =
      gst_pad_new_from_static_template (&gst_rtp_jitter_buffer_sink_template,
      "sink");

  gst_pad_set_chain_function (priv->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_chain));
  gst_pad_set_event_function (priv->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_sink_event));
  gst_pad_set_setcaps_function (priv->sinkpad,
      GST_DEBUG_FUNCPTR (gst_jitter_buffer_sink_setcaps));
  gst_pad_set_getcaps_function (priv->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_jitter_buffer_getcaps));

  gst_element_add_pad (GST_ELEMENT (jitterbuffer), priv->srcpad);
  gst_element_add_pad (GST_ELEMENT (jitterbuffer), priv->sinkpad);
}

static void
gst_rtp_jitter_buffer_dispose (GObject * object)
{
  GstRtpJitterBuffer *jitterbuffer;

  jitterbuffer = GST_RTP_JITTER_BUFFER (object);
  if (jitterbuffer->priv->jbuf) {
    g_object_unref (jitterbuffer->priv->jbuf);
    jitterbuffer->priv->jbuf = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_rtp_jitter_buffer_clear_pt_map (GstRtpJitterBuffer * jitterbuffer)
{
  GstRtpJitterBufferPrivate *priv;

  priv = jitterbuffer->priv;

  /* this will trigger a new pt-map request signal, FIXME, do something better. */
  priv->clock_rate = -1;
}

static GstCaps *
gst_rtp_jitter_buffer_getcaps (GstPad * pad)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;
  GstPad *other;
  GstCaps *caps;
  const GstCaps *templ;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));
  priv = jitterbuffer->priv;

  other = (pad == priv->srcpad ? priv->sinkpad : priv->srcpad);

  caps = gst_pad_peer_get_caps (other);

  templ = gst_pad_get_pad_template_caps (pad);
  if (caps == NULL) {
    GST_DEBUG_OBJECT (jitterbuffer, "copy template");
    caps = gst_caps_copy (templ);
  } else {
    GstCaps *intersect;

    GST_DEBUG_OBJECT (jitterbuffer, "intersect with template");

    intersect = gst_caps_intersect (caps, templ);
    gst_caps_unref (caps);

    caps = intersect;
  }
  gst_object_unref (jitterbuffer);

  return caps;
}

static gboolean
gst_jitter_buffer_sink_parse_caps (GstRtpJitterBuffer * jitterbuffer,
    GstCaps * caps)
{
  GstRtpJitterBufferPrivate *priv;
  GstStructure *caps_struct;
  guint val;

  priv = jitterbuffer->priv;

  /* first parse the caps */
  caps_struct = gst_caps_get_structure (caps, 0);

  GST_DEBUG_OBJECT (jitterbuffer, "got caps");

  /* we need a clock-rate to convert the rtp timestamps to GStreamer time and to
   * measure the amount of data in the buffer */
  if (!gst_structure_get_int (caps_struct, "clock-rate", &priv->clock_rate))
    goto error;

  if (priv->clock_rate <= 0)
    goto wrong_rate;

  GST_DEBUG_OBJECT (jitterbuffer, "got clock-rate %d", priv->clock_rate);

  /* gah, clock-base is uint. If we don't have a base, we will use the first
   * buffer timestamp as the base time. This will screw up sync but it's better
   * than nothing. */
  if (gst_structure_get_uint (caps_struct, "clock-base", &val))
    priv->clock_base = val;
  else
    priv->clock_base = -1;

  GST_DEBUG_OBJECT (jitterbuffer, "got clock-base %" G_GINT64_FORMAT,
      priv->clock_base);

  /* first expected seqnum */
  if (gst_structure_get_uint (caps_struct, "seqnum-base", &val))
    priv->next_seqnum = val;
  else
    priv->next_seqnum = -1;

  GST_DEBUG_OBJECT (jitterbuffer, "got seqnum-base %d", priv->next_seqnum);

  return TRUE;

  /* ERRORS */
error:
  {
    GST_DEBUG_OBJECT (jitterbuffer, "No clock-rate in caps!");
    return FALSE;
  }
wrong_rate:
  {
    GST_DEBUG_OBJECT (jitterbuffer, "Invalid clock-rate %d", priv->clock_rate);
    return FALSE;
  }
}

static gboolean
gst_jitter_buffer_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;
  gboolean res;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));
  priv = jitterbuffer->priv;

  res = gst_jitter_buffer_sink_parse_caps (jitterbuffer, caps);

  /* set same caps on srcpad on success */
  if (res)
    gst_pad_set_caps (priv->srcpad, caps);

  gst_object_unref (jitterbuffer);

  return res;
}

static void
gst_rtp_jitter_buffer_flush_start (GstRtpJitterBuffer * jitterbuffer)
{
  GstRtpJitterBufferPrivate *priv;

  priv = jitterbuffer->priv;

  JBUF_LOCK (priv);
  /* mark ourselves as flushing */
  priv->srcresult = GST_FLOW_WRONG_STATE;
  GST_DEBUG_OBJECT (jitterbuffer, "Disabling pop on queue");
  /* this unblocks any waiting pops on the src pad task */
  JBUF_SIGNAL (priv);
  rtp_jitter_buffer_flush (priv->jbuf);
  /* unlock clock, we just unschedule, the entry will be released by the 
   * locking streaming thread. */
  if (priv->clock_id)
    gst_clock_id_unschedule (priv->clock_id);
  JBUF_UNLOCK (priv);
}

static void
gst_rtp_jitter_buffer_flush_stop (GstRtpJitterBuffer * jitterbuffer)
{
  GstRtpJitterBufferPrivate *priv;

  priv = jitterbuffer->priv;

  JBUF_LOCK (priv);
  GST_DEBUG_OBJECT (jitterbuffer, "Enabling pop on queue");
  /* Mark as non flushing */
  priv->srcresult = GST_FLOW_OK;
  gst_segment_init (&priv->segment, GST_FORMAT_TIME);
  priv->last_popped_seqnum = -1;
  priv->next_seqnum = -1;
  priv->clock_rate = -1;
  priv->eos = FALSE;
  priv->exttimestamp = -1;
  JBUF_UNLOCK (priv);
}

static gboolean
gst_rtp_jitter_buffer_src_activate_push (GstPad * pad, gboolean active)
{
  gboolean result = TRUE;
  GstRtpJitterBuffer *jitterbuffer = NULL;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));

  if (active) {
    /* allow data processing */
    gst_rtp_jitter_buffer_flush_stop (jitterbuffer);

    /* start pushing out buffers */
    GST_DEBUG_OBJECT (jitterbuffer, "Starting task on srcpad");
    gst_pad_start_task (jitterbuffer->priv->srcpad,
        (GstTaskFunction) gst_rtp_jitter_buffer_loop, jitterbuffer);
  } else {
    /* make sure all data processing stops ASAP */
    gst_rtp_jitter_buffer_flush_start (jitterbuffer);

    /* NOTE this will hardlock if the state change is called from the src pad
     * task thread because we will _join() the thread. */
    GST_DEBUG_OBJECT (jitterbuffer, "Stopping task on srcpad");
    result = gst_pad_stop_task (pad);
  }

  gst_object_unref (jitterbuffer);

  return result;
}

static GstStateChangeReturn
gst_rtp_jitter_buffer_change_state (GstElement * element,
    GstStateChange transition)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  jitterbuffer = GST_RTP_JITTER_BUFFER (element);
  priv = jitterbuffer->priv;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      JBUF_LOCK (priv);
      /* reset negotiated values */
      priv->clock_rate = -1;
      priv->clock_base = -1;
      priv->peer_latency = 0;
      /* block until we go to PLAYING */
      priv->blocked = TRUE;
      priv->exttimestamp = -1;
      JBUF_UNLOCK (priv);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      JBUF_LOCK (priv);
      /* unblock to allow streaming in PLAYING */
      priv->blocked = FALSE;
      JBUF_SIGNAL (priv);
      JBUF_UNLOCK (priv);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* we are a live element because we sync to the clock, which we can only
       * do in the PLAYING state */
      if (ret != GST_STATE_CHANGE_FAILURE)
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      JBUF_LOCK (priv);
      /* block to stop streaming when PAUSED */
      priv->blocked = TRUE;
      JBUF_UNLOCK (priv);
      if (ret != GST_STATE_CHANGE_FAILURE)
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

/**
 * Performs comparison 'b - a' with check for overflows.
 */
static inline gint
priv_compare_rtp_seq_lt (guint16 a, guint16 b)
{
  /* check if diff more than half of the 16bit range */
  if (abs (b - a) > (1 << 15)) {
    /* one of a/b has wrapped */
    return a - b;
  } else {
    return b - a;
  }
}

static gboolean
gst_rtp_jitter_buffer_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean ret = TRUE;
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));
  priv = jitterbuffer->priv;

  GST_DEBUG_OBJECT (jitterbuffer, "received %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NEWSEGMENT:
    {
      GstFormat format;
      gdouble rate, arate;
      gint64 start, stop, time;
      gboolean update;

      gst_event_parse_new_segment_full (event, &update, &rate, &arate, &format,
          &start, &stop, &time);

      /* we need time for now */
      if (format != GST_FORMAT_TIME)
        goto newseg_wrong_format;

      GST_DEBUG_OBJECT (jitterbuffer,
          "newsegment: update %d, rate %g, arate %g, start %" GST_TIME_FORMAT
          ", stop %" GST_TIME_FORMAT ", time %" GST_TIME_FORMAT,
          update, rate, arate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop),
          GST_TIME_ARGS (time));

      /* now configure the values, we need these to time the release of the
       * buffers on the srcpad. */
      gst_segment_set_newsegment_full (&priv->segment, update,
          rate, arate, format, start, stop, time);

      /* FIXME, push SEGMENT in the queue. Sorting order might be difficult. */
      ret = gst_pad_push_event (priv->srcpad, event);
      break;
    }
    case GST_EVENT_FLUSH_START:
      gst_rtp_jitter_buffer_flush_start (jitterbuffer);
      ret = gst_pad_push_event (priv->srcpad, event);
      break;
    case GST_EVENT_FLUSH_STOP:
      ret = gst_pad_push_event (priv->srcpad, event);
      ret = gst_rtp_jitter_buffer_src_activate_push (priv->srcpad, TRUE);
      break;
    case GST_EVENT_EOS:
    {
      /* push EOS in queue. We always push it at the head */
      JBUF_LOCK (priv);
      /* check for flushing, we need to discard the event and return FALSE when
       * we are flushing */
      ret = priv->srcresult == GST_FLOW_OK;
      if (ret && !priv->eos) {
        GST_DEBUG_OBJECT (jitterbuffer, "queuing EOS");
        priv->eos = TRUE;
        JBUF_SIGNAL (priv);
      } else if (priv->eos) {
        GST_DEBUG_OBJECT (jitterbuffer, "dropping EOS, we are already EOS");
      } else {
        GST_DEBUG_OBJECT (jitterbuffer, "dropping EOS, reason %s",
            gst_flow_get_name (priv->srcresult));
      }
      JBUF_UNLOCK (priv);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_push_event (priv->srcpad, event);
      break;
  }

done:
  gst_object_unref (jitterbuffer);

  return ret;

  /* ERRORS */
newseg_wrong_format:
  {
    GST_DEBUG_OBJECT (jitterbuffer, "received non TIME newsegment");
    ret = FALSE;
    goto done;
  }
}

static gboolean
gst_rtp_jitter_buffer_get_clock_rate (GstRtpJitterBuffer * jitterbuffer,
    guint8 pt)
{
  GValue ret = { 0 };
  GValue args[2] = { {0}, {0} };
  GstCaps *caps;
  gboolean res;

  g_value_init (&args[0], GST_TYPE_ELEMENT);
  g_value_set_object (&args[0], jitterbuffer);
  g_value_init (&args[1], G_TYPE_UINT);
  g_value_set_uint (&args[1], pt);

  g_value_init (&ret, GST_TYPE_CAPS);
  g_value_set_boxed (&ret, NULL);

  g_signal_emitv (args, gst_rtp_jitter_buffer_signals[SIGNAL_REQUEST_PT_MAP], 0,
      &ret);

  caps = (GstCaps *) g_value_get_boxed (&ret);
  if (!caps)
    goto no_caps;

  res = gst_jitter_buffer_sink_parse_caps (jitterbuffer, caps);

  return res;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (jitterbuffer, "could not get caps");
    return FALSE;
  }
}

static GstFlowReturn
gst_rtp_jitter_buffer_chain (GstPad * pad, GstBuffer * buffer)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;
  guint16 seqnum;
  GstFlowReturn ret = GST_FLOW_OK;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));

  if (!gst_rtp_buffer_validate (buffer))
    goto invalid_buffer;

  priv = jitterbuffer->priv;

  if (priv->clock_rate == -1) {
    guint8 pt;

    /* no clock rate given on the caps, try to get one with the signal */
    pt = gst_rtp_buffer_get_payload_type (buffer);

    gst_rtp_jitter_buffer_get_clock_rate (jitterbuffer, pt);
    if (priv->clock_rate == -1)
      goto not_negotiated;
  }

  seqnum = gst_rtp_buffer_get_seq (buffer);
  GST_DEBUG_OBJECT (jitterbuffer, "Received packet #%d", seqnum);

  JBUF_LOCK_CHECK (priv, out_flushing);
  /* don't accept more data on EOS */
  if (priv->eos)
    goto have_eos;

  /* let's check if this buffer is too late, we cannot accept packets with
   * bigger seqnum than the one we already pushed. */
  if (priv->last_popped_seqnum != -1) {
    if (priv_compare_rtp_seq_lt (priv->last_popped_seqnum, seqnum) < 0)
      goto too_late;
  }

  /* let's drop oldest packet if the queue is already full and drop-on-latency
   * is set. We can only do this when there actually is a latency. When no
   * latency is set, we just pump it in the queue and let the other end push it
   * out as fast as possible. */
  if (priv->latency_ms && priv->drop_on_latency) {
    guint64 latency_ts;

    latency_ts =
        gst_util_uint64_scale_int (priv->latency_ms, priv->clock_rate, 1000);

    if (rtp_jitter_buffer_get_ts_diff (priv->jbuf) >= latency_ts) {
      GstBuffer *old_buf;

      GST_DEBUG_OBJECT (jitterbuffer, "Queue full, dropping old packet #%d",
          seqnum);

      old_buf = rtp_jitter_buffer_pop (priv->jbuf);
      gst_buffer_unref (old_buf);
    }
  }

  /* now insert the packet into the queue in sorted order. This function returns
   * FALSE if a packet with the same seqnum was already in the queue, meaning we
   * have a duplicate. */
  if (!rtp_jitter_buffer_insert (priv->jbuf, buffer))
    goto duplicate;

  /* signal addition of new buffer */
  JBUF_SIGNAL (priv);

  /* let's unschedule and unblock any waiting buffers. We only want to do this
   * if there is a currently waiting newer (> seqnum) buffer  */
  if (priv->clock_id) {
    if (priv->waiting_seqnum > seqnum) {
      gst_clock_id_unschedule (priv->clock_id);
      GST_DEBUG_OBJECT (jitterbuffer, "Unscheduling waiting buffer");
    }
  }

  GST_DEBUG_OBJECT (jitterbuffer, "Pushed packet #%d, now %d packets",
      seqnum, rtp_jitter_buffer_num_packets (priv->jbuf));

finished:
  JBUF_UNLOCK (priv);

  gst_object_unref (jitterbuffer);

  return ret;

  /* ERRORS */
invalid_buffer:
  {
    /* this is fatal and should be filtered earlier */
    GST_ELEMENT_ERROR (jitterbuffer, STREAM, DECODE, (NULL),
        ("Received invalid RTP payload"));
    gst_buffer_unref (buffer);
    gst_object_unref (jitterbuffer);
    return GST_FLOW_ERROR;
  }
not_negotiated:
  {
    GST_WARNING_OBJECT (jitterbuffer, "No clock-rate in caps!");
    gst_buffer_unref (buffer);
    gst_object_unref (jitterbuffer);
    return GST_FLOW_NOT_NEGOTIATED;
  }
out_flushing:
  {
    ret = priv->srcresult;
    GST_DEBUG_OBJECT (jitterbuffer, "flushing %s", gst_flow_get_name (ret));
    gst_buffer_unref (buffer);
    goto finished;
  }
have_eos:
  {
    ret = GST_FLOW_UNEXPECTED;
    GST_WARNING_OBJECT (jitterbuffer, "we are EOS, refusing buffer");
    gst_buffer_unref (buffer);
    goto finished;
  }
too_late:
  {
    GST_WARNING_OBJECT (jitterbuffer, "Packet #%d too late as #%d was already"
        " popped, dropping", seqnum, priv->last_popped_seqnum);
    priv->num_late++;
    gst_buffer_unref (buffer);
    goto finished;
  }
duplicate:
  {
    GST_WARNING_OBJECT (jitterbuffer, "Duplicate packet #%d detected, dropping",
        seqnum);
    priv->num_duplicates++;
    gst_buffer_unref (buffer);
    goto finished;
  }
}

/**
 * This funcion will push out buffers on the source pad.
 *
 * For each pushed buffer, the seqnum is recorded, if the next buffer B has a
 * different seqnum (missing packets before B), this function will wait for the
 * missing packet to arrive up to the rtp timestamp of buffer B.
 */
static void
gst_rtp_jitter_buffer_loop (GstRtpJitterBuffer * jitterbuffer)
{
  GstRtpJitterBufferPrivate *priv;
  GstBuffer *outbuf = NULL;
  GstFlowReturn result;
  guint16 seqnum;
  guint32 rtp_time;
  GstClockTime timestamp;
  gint64 running_time;
  guint64 exttimestamp;
  gint ts_offset_rtp;

  priv = jitterbuffer->priv;

  JBUF_LOCK_CHECK (priv, flushing);
again:
  GST_DEBUG_OBJECT (jitterbuffer, "Popping item");
  while (TRUE) {

    /* always wait if we are blocked */
    if (!priv->blocked) {
      /* if we have a packet, we can grab it */
      if (rtp_jitter_buffer_num_packets (priv->jbuf) > 0)
        break;
      /* no packets but we are EOS, do eos logic */
      if (priv->eos)
        goto do_eos;
    }
    /* wait for packets or flushing now */
    JBUF_WAIT_CHECK (priv, flushing);
  }

  /* pop a buffer, we must have a buffer now */
  outbuf = rtp_jitter_buffer_pop (priv->jbuf);

  seqnum = gst_rtp_buffer_get_seq (outbuf);

  /* get the max deadline to wait for the missing packets, this is the time
   * of the currently popped packet */
  rtp_time = gst_rtp_buffer_get_timestamp (outbuf);
  exttimestamp = gst_rtp_buffer_ext_timestamp (&priv->exttimestamp, rtp_time);

  GST_DEBUG_OBJECT (jitterbuffer,
      "Popped buffer #%d, rtptime %u, exttime %" G_GUINT64_FORMAT
      ",now %d left", seqnum, rtp_time, exttimestamp,
      rtp_jitter_buffer_num_packets (priv->jbuf));

  /* If we don't know what the next seqnum should be (== -1) we have to wait
   * because it might be possible that we are not receiving this buffer in-order,
   * a buffer with a lower seqnum could arrive later and we want to push that
   * earlier buffer before this buffer then.
   * If we know the expected seqnum, we can compare it to the current seqnum to
   * determine if we have missing a packet. If we have a missing packet (which
   * must be before this packet) we can wait for it until the deadline for this
   * packet expires. */
  if (priv->next_seqnum == -1 || priv->next_seqnum != seqnum) {
    GstClockID id;
    GstClockTimeDiff jitter;
    GstClockReturn ret;
    GstClock *clock;

    if (priv->next_seqnum != -1) {
      /* we expected next_seqnum but received something else, that's a gap */
      GST_WARNING_OBJECT (jitterbuffer,
          "Sequence number GAP detected -> %d instead of %d", priv->next_seqnum,
          seqnum);
    } else {
      /* we don't know what the next_seqnum should be, wait for the last
       * possible moment to push this buffer, maybe we get an earlier seqnum
       * while we wait */
      GST_DEBUG_OBJECT (jitterbuffer, "First buffer %d, do sync", seqnum);
    }

    GST_DEBUG_OBJECT (jitterbuffer,
        "exttimestamp %" G_GUINT64_FORMAT ", base %" G_GINT64_FORMAT,
        exttimestamp, priv->clock_base);

    /* if no clock_base was given, take first ts as base */
    if (priv->clock_base == -1) {
      GST_DEBUG_OBJECT (jitterbuffer,
          "no clock base, using exttimestamp %" G_GUINT64_FORMAT, exttimestamp);
      priv->clock_base = exttimestamp;
    }

    /* take rtp timestamp offset into account, this can wrap around */
    exttimestamp -= priv->clock_base;

    /* bring timestamp to gst time */
    timestamp =
        gst_util_uint64_scale_int (exttimestamp, GST_SECOND, priv->clock_rate);

    GST_DEBUG_OBJECT (jitterbuffer,
        "exttimestamp %" G_GUINT64_FORMAT ", clock-rate %u, timestamp %"
        GST_TIME_FORMAT, exttimestamp, priv->clock_rate,
        GST_TIME_ARGS (timestamp));

    /* bring to running time */
    running_time = gst_segment_to_running_time (&priv->segment, GST_FORMAT_TIME,
        timestamp);

    GST_OBJECT_LOCK (jitterbuffer);
    clock = GST_ELEMENT_CLOCK (jitterbuffer);
    if (!clock) {
      GST_OBJECT_UNLOCK (jitterbuffer);
      /* let's just push if there is no clock */
      goto push_buffer;
    }

    /* add latency, this includes our own latency and the peer latency. */
    running_time += (priv->latency_ms * GST_MSECOND);
    running_time += priv->peer_latency;

    GST_DEBUG_OBJECT (jitterbuffer, "sync to running_time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (running_time));

    /* prepare for sync against clock */
    running_time += GST_ELEMENT_CAST (jitterbuffer)->base_time;

    /* create an entry for the clock */
    id = priv->clock_id = gst_clock_new_single_shot_id (clock, running_time);
    priv->waiting_seqnum = seqnum;
    GST_OBJECT_UNLOCK (jitterbuffer);

    /* release the lock so that the other end can push stuff or unlock */
    JBUF_UNLOCK (priv);

    ret = gst_clock_id_wait (id, &jitter);

    JBUF_LOCK (priv);
    /* and free the entry */
    gst_clock_id_unref (id);
    priv->clock_id = NULL;
    priv->waiting_seqnum = -1;

    /* at this point, the clock could have been unlocked by a timeout, a new
     * tail element was added to the queue or because we are shutting down. Check
     * for shutdown first. */
    if (priv->srcresult != GST_FLOW_OK)
      goto flushing;

    /* if we got unscheduled and we are not flushing, it's because a new tail
     * element became available in the queue. Grab it and try to push or sync. */
    if (ret == GST_CLOCK_UNSCHEDULED) {
      GST_DEBUG_OBJECT (jitterbuffer,
          "Wait got unscheduled, will retry to push with new buffer");
      /* reinsert popped buffer into queue */
      if (!rtp_jitter_buffer_insert (priv->jbuf, outbuf)) {
        GST_DEBUG_OBJECT (jitterbuffer,
            "Duplicate packet #%d detected, dropping", seqnum);
        priv->num_duplicates++;
        gst_buffer_unref (outbuf);
      }
      goto again;
    }
  }
push_buffer:
  /* check if we are pushing something unexpected */
  if (priv->next_seqnum != -1 && priv->next_seqnum != seqnum) {
    gint dropped;

    /* calc number of missing packets, careful for wraparounds */
    dropped = priv_compare_rtp_seq_lt (priv->next_seqnum, seqnum);

    GST_DEBUG_OBJECT (jitterbuffer,
        "Pushing DISCONT after dropping %d (%d to %d)", dropped,
        priv->next_seqnum, seqnum);

    /* update stats */
    priv->num_late += dropped;

    /* set DISCONT flag */
    outbuf = gst_buffer_make_metadata_writable (outbuf);
    GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
  }

  /* apply the timestamp offset */
  if (priv->ts_offset > 0)
    ts_offset_rtp =
        gst_util_uint64_scale_int (priv->ts_offset, priv->clock_rate,
        GST_SECOND);
  else if (priv->ts_offset < 0)
    ts_offset_rtp =
        -gst_util_uint64_scale_int (-priv->ts_offset, priv->clock_rate,
        GST_SECOND);
  else
    ts_offset_rtp = 0;

  if (ts_offset_rtp != 0) {
    guint32 timestamp;

    /* if the offset changed, mark with discont */
    if (priv->ts_offset != priv->prev_ts_offset) {
      GST_DEBUG_OBJECT (jitterbuffer, "changing offset to %d", ts_offset_rtp);
      GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
      priv->prev_ts_offset = priv->ts_offset;
    }

    timestamp = gst_rtp_buffer_get_timestamp (outbuf);
    timestamp += ts_offset_rtp;
    gst_rtp_buffer_set_timestamp (outbuf, timestamp);
  }

  /* now we are ready to push the buffer. Save the seqnum and release the lock
   * so the other end can push stuff in the queue again. */
  priv->last_popped_seqnum = seqnum;
  priv->next_seqnum = (seqnum + 1) & 0xffff;
  JBUF_UNLOCK (priv);

  /* push buffer */
  GST_DEBUG_OBJECT (jitterbuffer, "Pushing buffer %d", seqnum);
  result = gst_pad_push (priv->srcpad, outbuf);
  if (result != GST_FLOW_OK)
    goto pause;

  return;

  /* ERRORS */
do_eos:
  {
    /* store result, we are flushing now */
    GST_DEBUG_OBJECT (jitterbuffer, "We are EOS, pushing EOS downstream");
    priv->srcresult = GST_FLOW_UNEXPECTED;
    gst_pad_pause_task (priv->srcpad);
    gst_pad_push_event (priv->srcpad, gst_event_new_eos ());
    JBUF_UNLOCK (priv);
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (jitterbuffer, "we are flushing");
    gst_pad_pause_task (priv->srcpad);
    if (outbuf)
      gst_buffer_unref (outbuf);
    JBUF_UNLOCK (priv);
    return;
  }
pause:
  {
    const gchar *reason = gst_flow_get_name (result);

    GST_DEBUG_OBJECT (jitterbuffer, "pausing task, reason %s", reason);

    JBUF_LOCK (priv);
    /* store result */
    priv->srcresult = result;
    /* we don't post errors or anything because upstream will do that for us
     * when we pass the return value upstream. */
    gst_pad_pause_task (priv->srcpad);
    JBUF_UNLOCK (priv);
    return;
  }
}

static gboolean
gst_rtp_jitter_buffer_query (GstPad * pad, GstQuery * query)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;
  gboolean res = FALSE;

  jitterbuffer = GST_RTP_JITTER_BUFFER (gst_pad_get_parent (pad));
  priv = jitterbuffer->priv;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      /* We need to send the query upstream and add the returned latency to our
       * own */
      GstClockTime min_latency, max_latency;
      gboolean us_live;
      GstPad *peer;
      GstClockTime our_latency;

      if ((peer = gst_pad_get_peer (priv->sinkpad))) {
        if ((res = gst_pad_query (peer, query))) {
          gst_query_parse_latency (query, &us_live, &min_latency, &max_latency);

          GST_DEBUG_OBJECT (jitterbuffer, "Peer latency: min %"
              GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
              GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));

          /* store this so that we can safely sync on the peer buffers. */
          JBUF_LOCK (priv);
          priv->peer_latency = min_latency;
          JBUF_UNLOCK (priv);

          our_latency = ((guint64) priv->latency_ms) * GST_MSECOND;

          GST_DEBUG_OBJECT (jitterbuffer, "Our latency: %" GST_TIME_FORMAT,
              GST_TIME_ARGS (our_latency));

          min_latency += our_latency;
          /* max_latency can be -1, meaning there is no upper limit for the
           * latency. */
          if (max_latency != -1)
            max_latency += our_latency * GST_MSECOND;

          GST_DEBUG_OBJECT (jitterbuffer, "Calculated total latency : min %"
              GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
              GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));

          gst_query_set_latency (query, TRUE, min_latency, max_latency);
        }
        gst_object_unref (peer);
      }
      break;
    }
    default:
      res = gst_pad_query_default (pad, query);
      break;
  }
  return res;
}

static void
gst_rtp_jitter_buffer_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;

  jitterbuffer = GST_RTP_JITTER_BUFFER (object);
  priv = jitterbuffer->priv;

  switch (prop_id) {
    case PROP_LATENCY:
    {
      guint new_latency, old_latency;

      /* FIXME, not threadsafe */
      new_latency = g_value_get_uint (value);
      old_latency = priv->latency_ms;

      priv->latency_ms = new_latency;

      /* post message if latency changed, this will inform the parent pipeline
       * that a latency reconfiguration is possible/needed. */
      if (new_latency != old_latency) {
        GST_DEBUG_OBJECT (jitterbuffer, "latency changed to: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (new_latency * GST_MSECOND));

        gst_element_post_message (GST_ELEMENT_CAST (jitterbuffer),
            gst_message_new_latency (GST_OBJECT_CAST (jitterbuffer)));
      }
      break;
    }
    case PROP_DROP_ON_LATENCY:
      priv->drop_on_latency = g_value_get_boolean (value);
      break;
    case PROP_TS_OFFSET:
      JBUF_LOCK (priv);
      priv->ts_offset = g_value_get_int64 (value);
      JBUF_UNLOCK (priv);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_jitter_buffer_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRtpJitterBuffer *jitterbuffer;
  GstRtpJitterBufferPrivate *priv;

  jitterbuffer = GST_RTP_JITTER_BUFFER (object);
  priv = jitterbuffer->priv;

  switch (prop_id) {
    case PROP_LATENCY:
      g_value_set_uint (value, priv->latency_ms);
      break;
    case PROP_DROP_ON_LATENCY:
      g_value_set_boolean (value, priv->drop_on_latency);
      break;
    case PROP_TS_OFFSET:
      JBUF_LOCK (priv);
      g_value_set_int64 (value, priv->ts_offset);
      JBUF_UNLOCK (priv);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
