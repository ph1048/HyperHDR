#pragma once

#include <led-drivers/LedDevice.h>

#include <vector>

class DriverHidRobobloq : public LedDevice
{
public:
	explicit DriverHidRobobloq(const QJsonObject& deviceConfig);
	static LedDevice* construct(const QJsonObject& deviceConfig);

protected:

	bool init(QJsonObject deviceConfig) override;
	int open() override;
	int close() override;
	int writeFiniteColors(const std::vector<ColorRgb>& ledValues) override;

private:
	bool hidReportFull() const;
	int flushHidReport();
	int writeReport(const std::vector<quint8>& data);

	quint16 _vid;
	quint16 _pid;
	int _ledCount;
	void* _handle;
	void* _evhandle;
	int _reportLen;
	std::vector<quint8> _report;
	static bool isRegistered;
};
