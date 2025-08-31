#include <QPainter>
#include <QTimer>
#include "media-io/audio-math.h"
#include "AudioFilterWidget.hpp"
#include "audio-filter-widget.h"

AudioFilterWidget::AudioFilterWidget(obs_source_t *source, QWidget *parent)
	: QWidget(parent),
	weak_source(OBSGetWeakRef(source))
{
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "update", AudioFilterWidget::on_update_cb, this);

	setMinimumSize(128, 128);
}

AudioFilterWidget::~AudioFilterWidget()
{
	if (OBSSourceAutoRelease source = OBSGetStrongRef(weak_source)) {
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_disconnect(sh, "update", AudioFilterWidget::on_update_cb, this);
	}
}

void AudioFilterWidget::on_update(calldata_t *)
{
}

void AudioFilterWidget::on_update_cb(void *data, calldata_t *cd)
{
	auto *inst = static_cast<AudioFilterWidget *>(data);
	if (!inst)
		return;

	inst->on_update(cd);
}

CompressorWidget::CompressorWidget(obs_source_t *source, QWidget *parent)
	: AudioFilterWidget(source, parent)
{
	load_settings(source);

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &CompressorWidget::on_timer);
	timer->start(45);
}

void CompressorWidget::on_timer()
{
	uint8_t stack[128];
	calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));

	OBSSourceAutoRelease source = OBSGetStrongRef(weak_source);
	if (!source)
		return;

	/* TODO: Need to consider multi-thread safety.
	 * It would be better to send the data from the filter using a signal.
	 */
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_call(ph, "current_state", &cd);
	envelope = calldata_float(&cd, "envelope");

	update();

}

void CompressorWidget::on_update(calldata_t *cd)
{
	obs_source_t *source;
	if (!calldata_get_ptr(cd, "source", &source))
		return;
	if (!source)
		return;
	load_settings(source);
}

void CompressorWidget::load_settings(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);

	threshold = (float)obs_data_get_double(settings, "threshold");
	slope = 1.0f - (1.0f / (float)obs_data_get_double(settings, "ratio"));
	output_gain = (float)obs_data_get_double(settings, "output_gain");

	QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
}

void CompressorWidget::paintEvent(QPaintEvent *)
{
	float x0 = -60.0f;
	float y0 = -60.0f + output_gain;

	float x1 = threshold;
	float y1 = threshold + output_gain;

	float x2 = 0.0f;
	// float y2 = threshold + (1-slope) * (0.0f - threshold) + output_gain;
	float y2 = slope * threshold + output_gain;

	/* TODO: Instead of displaying envelope, it should calculate current input and output levels.  */
	float xe = mul_to_db(envelope);
	float ye = (xe > threshold ? + xe - slope * (xe - threshold) : xe) + output_gain;

	QRect widgetRect = rect();
	const int width = widgetRect.width();
	const int height = widgetRect.height();

	auto toPoint = [width, height](float x, float y) {
		int px = (int)((x + 60.0f) * width / 60.0f);
		int py = height - (int)((y + 60.0f) * height / 60.0f);
		return QPoint(px, py);
	};

	QPainter painter(this);

	painter.drawLine(toPoint(x0, y0), toPoint(x1, y1));
	painter.drawLine(toPoint(x1, y1), toPoint(x2, y2));

	painter.drawLine(toPoint(xe, -60.0f), toPoint(xe, ye));
	painter.drawLine(toPoint(xe, ye), toPoint(-60.0f, ye));
}

void *get_compressor_widget(obs_source_t *source, void *parent)
{
	return new CompressorWidget(source, static_cast<QWidget *>(parent));
}
