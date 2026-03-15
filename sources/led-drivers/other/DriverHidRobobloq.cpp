#include <led-drivers/other/DriverHidRobobloq.h>

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <vector>

DriverHidRobobloq::DriverHidRobobloq(const QJsonObject& deviceConfig)
	: LedDevice(deviceConfig), _handle(INVALID_HANDLE_VALUE), _evhandle(nullptr)
{	
}

static quint16 parseHexVidpid(const QString& val) {
    QString s = val.trimmed();
    if (s.startsWith("0x", Qt::CaseInsensitive))
        s.remove(0, 2);

    bool ok = false;
    unsigned int n = s.toUInt(&ok, 16);
    if (!ok || n > 0xFFFFu)
        return 0;

    return static_cast<quint16>(n);
}

bool DriverHidRobobloq::init(QJsonObject deviceConfig)
{
	if (!LedDevice::init(deviceConfig))
		return false;

	_vid = parseHexVidpid(deviceConfig["VID"].toString(""));
	if (!_vid)
		return false;
	_pid = parseHexVidpid(deviceConfig["PID"].toString(""));
	if (!_pid)
		return false;
	
	_ledCount = deviceConfig["currentLedCount"].toInt(0);
	if (!_ledCount)
		return false;

	setLedCount(_ledCount);

	Debug(_log, "Flamez init OK with led count {:d}", (_ledCount));

	return true;
}

int DriverHidRobobloq::open()
{
	int ret = -1;
	do
	{
		GUID hidGuid;
		HidD_GetHidGuid(&hidGuid);

		HDEVINFO hDevInfo = SetupDiGetClassDevs(
			&hidGuid, nullptr, nullptr,
			DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
		if (hDevInfo == INVALID_HANDLE_VALUE)
			break;

		SP_DEVICE_INTERFACE_DATA ifData = {};
		ifData.cbSize = sizeof(ifData);

		HANDLE h = INVALID_HANDLE_VALUE;

		// enumerate all HID interfaces
		for (DWORD idx = 0; ; ++idx)
		{
			if (h != INVALID_HANDLE_VALUE) {
				CloseHandle(h);
				h = INVALID_HANDLE_VALUE;
			}
			if (!SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, idx, &ifData))
				break;
			// ask for the needed detail buffer size
			DWORD needed = 0;
			SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, nullptr, 0, &needed, nullptr);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
				continue;

			// retrieve the device path
			auto detailBuf = std::vector<uint8_t>(needed);
			auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA*)detailBuf.data();
			detail->cbSize = sizeof(*detail);
			if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, detail, needed, nullptr, nullptr))
				continue;

			// try to open it
			h = CreateFile(
				detail->DevicePath,
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED,
				nullptr);
			if (h == INVALID_HANDLE_VALUE)
				continue;

			// check VID/PID
			HIDD_ATTRIBUTES attrib = { sizeof(attrib) };
			if (!HidD_GetAttributes(h, &attrib)
				|| attrib.VendorID  != static_cast<USHORT>(_vid)
				|| attrib.ProductID != static_cast<USHORT>(_pid))
				continue;

			// get the OUT‐report length
			PHIDP_PREPARSED_DATA ppd = nullptr;
			if (!HidD_GetPreparsedData(h, &ppd))
				continue;
			
			HIDP_CAPS caps;
			bool ok = (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS);
			HidD_FreePreparsedData(ppd);
			if (!ok || caps.OutputReportByteLength == 0)
				continue;

			ret = 0;
			_handle = h;
			_reportLen = static_cast<int>(caps.OutputReportByteLength);
			
			_report.reserve(caps.OutputReportByteLength);
			break;
		}

		SetupDiDestroyDeviceInfoList(hDevInfo);
	} while(0);
	
	if (ret)
		return ret;

    _evhandle = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!_evhandle)
		return -1;

    return LedDevice::open();
}

int DriverHidRobobloq::close()
{
	int ret = LedDevice::close();
	if (ret)
		return ret;

	if (_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(_handle);
		_handle = INVALID_HANDLE_VALUE;
	}
	if (_evhandle != nullptr) {
		CloseHandle(_evhandle);
		_evhandle = nullptr;
	}
	return 0;
}


bool DriverHidRobobloq::hidReportFull() const
{
	return _report.size() >= (_reportLen - 1);
}

int DriverHidRobobloq::flushHidReport()
{
    do {
        std::vector<quint8> buf(_reportLen);
        size_t toSend = min((_reportLen - 1), _report.size());
        std::copy(_report.begin(), _report.begin() + toSend, buf.begin() + 1);
        _report.erase(_report.begin(), _report.begin() + toSend);

        OVERLAPPED ov = {};
        ov.hEvent = _evhandle;
        DWORD written = 0;
        WriteFile(
            _handle,
            buf.data(),
            (DWORD)buf.size(),
            &written,
            &ov);
        if (!GetOverlappedResult(_handle, &ov, &written, TRUE)) {
            return -1;
        }
    }
	while (hidReportFull());

    return 0;
}

int DriverHidRobobloq::writeReport(const std::vector<quint8>& data)
{
	_report.insert(_report.end(), data.begin(), data.end());
    if (hidReportFull()) {
        if (flushHidReport()) {
			return -1;
		}
    }
	return 0;
}

int DriverHidRobobloq::writeFiniteColors(const std::vector<ColorRgb>& ledValues)
{
	const auto RobobloqProtoUpdateLedHeader = std::vector<quint8>({0x53, 0x43, 0x01, 0x01, 0xff});
	int ret = writeReport(RobobloqProtoUpdateLedHeader);
	if (ret)
		return ret;
	
	for (int i = 0; i < ledValues.size(); ++i)
	{
		quint8 r = ledValues[i].red;
		quint8 g = ledValues[i].green;
		quint8 b = ledValues[i].blue;

		const quint8 RobobloqProtoLedIndexCurrent = 0x80;
		const int RobobloqProtoLedIndexGrouping = 2;
		const int RobobloqProtoLedIndexOffset = 1;

		ret = writeReport({RobobloqProtoLedIndexCurrent, static_cast<quint8>(RobobloqProtoLedIndexGrouping * i + RobobloqProtoLedIndexOffset), r, g, b});
		if (ret)
			return ret;
	}
	return flushHidReport();
}

LedDevice* DriverHidRobobloq::construct(const QJsonObject& deviceConfig)
{
	return new DriverHidRobobloq(deviceConfig);
}

bool DriverHidRobobloq::isRegistered = hyperhdr::leds::REGISTER_LED_DEVICE("robobloq", "leds_group_3_serial", DriverHidRobobloq::construct);
