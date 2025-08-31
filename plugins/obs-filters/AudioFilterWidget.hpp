#pragma once

#include <QWidget>
#include "obs.hpp"

class AudioFilterWidget : public QWidget
{
	Q_OBJECT

public:
	AudioFilterWidget(obs_source_t *source, QWidget *parent = 0);
	~AudioFilterWidget() override;

protected:
	virtual void on_update(calldata_t *cd);

private:
	static void on_update_cb(void *data, calldata_t *cd);

protected:
	OBSWeakSource weak_source;
};

class CompressorWidget : public AudioFilterWidget
{
	Q_OBJECT

public:
	CompressorWidget(obs_source_t *source, QWidget *parent = 0);

protected:
	void on_update(calldata_t *cd) override;
	void paintEvent(class QPaintEvent *event) override;

private:
	void on_timer();

	void load_settings(obs_source_t *source);

private:
	/* current state */
	float envelope = 0.0f;

	/* settings */
	float threshold = 0.0f;
	float slope = 0.0f;
	float output_gain = 0.0f;
};
