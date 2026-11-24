#include "core/panorama-builder.hpp"
#include "core/panorama-store.hpp"
#include "onvif/onvif-auth.hpp"
#include "onvif/onvif-client.hpp"
#include "onvif/ws-discovery.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace netdev;

namespace {

int g_failures = 0;

void check(bool cond, const char *expr, int line)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL [line %d]: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

void testAuth()
{
    const QByteArray rawNonce("0123456789abcdef");
    const QString created = QStringLiteral("2026-11-20T12:00:00Z");
    const QString digest = OnvifAuth::buildPasswordDigest(rawNonce, created, QStringLiteral("secret123"));
    CHECK(digest == QStringLiteral("51Gb1RBkVT1jnvM9IyU9r+DjL64="));

    const QString secXml = OnvifAuth::buildWsSecurityHeader(QStringLiteral("admin&1"), QStringLiteral("p<ass>"), 0);
    CHECK(secXml.contains(QStringLiteral("admin&amp;1")));
    CHECK(secXml.contains(QStringLiteral("<wsse:Password Type=")));
    CHECK(secXml.contains(QStringLiteral("<wsse:Nonce EncodingType=")));
    CHECK(secXml.contains(QStringLiteral("<wsu:Created>")));

    const QString injected = OnvifAuth::injectRtspCredentials(
        QStringLiteral("rtsp://192.168.1.50:554/Streaming/Channels/101"),
        QStringLiteral("admin"),
        QStringLiteral("p@ss:word#1"));
    CHECK(injected == QStringLiteral("rtsp://admin:p%40ss%3Aword%231@192.168.1.50:554/Streaming/Channels/101"));

    // Idempotent when credentials already exist in URL
    const QString already = OnvifAuth::injectRtspCredentials(
        QStringLiteral("rtsp://user1:pass1@192.168.1.50:554/stream1"),
        QStringLiteral("other"),
        QStringLiteral("otherpass"));
    CHECK(already == QStringLiteral("rtsp://user1:pass1@192.168.1.50:554/stream1"));
}

void testWsDiscoveryParser()
{
    const QByteArray hikResponse = R"(<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="http://www.w3.org/2003/05/soap-envelope"
              xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
              xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery">
  <env:Body>
    <d:ProbeMatches>
      <d:ProbeMatch>
        <wsa:EndpointReference>
          <wsa:Address>urn:uuid:11223344-5566-7788-99aa-bbccddeeff00</wsa:Address>
        </wsa:EndpointReference>
        <d:Types>dn:NetworkVideoTransmitter</d:Types>
        <d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/ptz onvif://www.onvif.org/hardware/DS-2DE4225IW-DE onvif://www.onvif.org/name/HIKVISION%20PTZ%20Dome</d:Scopes>
        <d:XAddrs>http://[fe80::1]/onvif/device_service http://192.168.1.64:8080/onvif/device_service</d:XAddrs>
      </d:ProbeMatch>
    </d:ProbeMatches>
  </env:Body>
</env:Envelope>)";

    const QVector<DiscoveredDevice> devs = WsDiscovery::parseProbeMatchXml(hikResponse, QStringLiteral("192.168.1.64"));
    CHECK(devs.size() == 1);
    if (devs.size() == 1) {
        const DiscoveredDevice &d = devs.at(0);
        CHECK(d.ip == QStringLiteral("192.168.1.64"));
        CHECK(d.port == 8080);
        CHECK(d.deviceServiceUrl == QStringLiteral("http://192.168.1.64:8080/onvif/device_service"));
        CHECK(d.name == QStringLiteral("HIKVISION PTZ Dome"));
        CHECK(d.model == QStringLiteral("DS-2DE4225IW-DE"));
        CHECK(d.hasPtz == true);
    }

    const QByteArray genericResponse = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:a="http://schemas.xmlsoap.org/ws/2004/08/addressing"
            xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery">
  <s:Body>
    <wsd:ProbeMatches>
      <wsd:ProbeMatch>
        <a:EndpointReference>
          <a:Address>urn:uuid:aaaa-bbbb</a:Address>
        </a:EndpointReference>
        <wsd:Scopes>onvif://www.onvif.org/name/Tapo%20C520WS</wsd:Scopes>
        <wsd:XAddrs>http://192.168.1.88:2020/onvif/device_service</wsd:XAddrs>
      </wsd:ProbeMatch>
    </wsd:ProbeMatches>
  </s:Body>
</s:Envelope>)";

    const QVector<DiscoveredDevice> devs2 = WsDiscovery::parseProbeMatchXml(genericResponse);
    CHECK(devs2.size() == 1);
    if (devs2.size() == 1) {
        CHECK(devs2.at(0).port == 2020);
        CHECK(devs2.at(0).name == QStringLiteral("Tapo C520WS"));
        CHECK(devs2.at(0).hasPtz == false);
    }
}

void testSystemDateAndTimeParser()
{
    const QByteArray dtXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
            xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tds:GetSystemDateAndTimeResponse>
      <tds:SystemDateAndTime>
        <tt:UTCDateTime>
          <tt:Time><tt:Hour>12</tt:Hour><tt:Minute>05</tt:Minute><tt:Second>30</tt:Second></tt:Time>
          <tt:Date><tt:Year>2026</tt:Year><tt:Month>11</tt:Month><tt:Day>20</tt:Day></tt:Date>
        </tt:UTCDateTime>
      </tds:SystemDateAndTime>
    </tds:GetSystemDateAndTimeResponse>
  </s:Body>
</s:Envelope>)";

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const QDateTime localUtc(QDate(2026, 11, 20), QTime(12, 0, 0), QTimeZone::utc());
#else
    const QDateTime localUtc(QDate(2026, 11, 20), QTime(12, 0, 0), Qt::UTC);
#endif
    qint64 offset = 0;
    CHECK(OnvifClient::parseSystemDateAndTime(dtXml, offset, localUtc));
    CHECK(offset == 330);
}

void testDeviceInfoServicesCapabilities()
{
    const QByteArray infoXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl">
  <s:Body>
    <tds:GetDeviceInformationResponse>
      <tds:Manufacturer>Dahua</tds:Manufacturer>
      <tds:Model>IPC-HDW2431T</tds:Model>
      <tds:FirmwareVersion>2.820.0000</tds:FirmwareVersion>
      <tds:SerialNumber>SN123456</tds:SerialNumber>
    </tds:GetDeviceInformationResponse>
  </s:Body>
</s:Envelope>)";

    DiscoveredDevice dev;
    CHECK(OnvifClient::parseDeviceInformation(infoXml, dev));
    CHECK(dev.manufacturer == QStringLiteral("Dahua"));
    CHECK(dev.model == QStringLiteral("IPC-HDW2431T"));
    CHECK(dev.firmwareVersion == QStringLiteral("2.820.0000"));
    CHECK(dev.serialNumber == QStringLiteral("SN123456"));
    CHECK(dev.name == QStringLiteral("Dahua IPC-HDW2431T"));

    const QByteArray servicesXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl">
  <s:Body>
    <tds:GetServicesResponse>
      <tds:Service>
        <tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>
        <tds:XAddr>http://192.168.1.50/onvif/media_service</tds:XAddr>
      </tds:Service>
      <tds:Service>
        <tds:Namespace>http://www.onvif.org/ver20/media/wsdl</tds:Namespace>
        <tds:XAddr>http://192.168.1.50/onvif/media2_service</tds:XAddr>
      </tds:Service>
      <tds:Service>
        <tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace>
        <tds:XAddr>http://192.168.1.50/onvif/ptz_service</tds:XAddr>
      </tds:Service>
    </tds:GetServicesResponse>
  </s:Body>
</s:Envelope>)";

    QString m1, m2, ptz;
    CHECK(OnvifClient::parseServices(servicesXml, m1, m2, ptz));
    CHECK(m1 == QStringLiteral("http://192.168.1.50/onvif/media_service"));
    CHECK(m2 == QStringLiteral("http://192.168.1.50/onvif/media2_service"));
    CHECK(ptz == QStringLiteral("http://192.168.1.50/onvif/ptz_service"));

    const QByteArray capXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tds:GetCapabilitiesResponse>
      <tds:Capabilities>
        <tt:Media><tt:XAddr>http://192.168.1.77:8899/onvif/media</tt:XAddr></tt:Media>
        <tt:PTZ><tt:XAddr>http://192.168.1.77:8899/onvif/ptz</tt:XAddr></tt:PTZ>
      </tds:Capabilities>
    </tds:GetCapabilitiesResponse>
  </s:Body>
</s:Envelope>)";

    QString capMedia, capPtz;
    CHECK(OnvifClient::parseCapabilities(capXml, capMedia, capPtz));
    CHECK(capMedia == QStringLiteral("http://192.168.1.77:8899/onvif/media"));
    CHECK(capPtz == QStringLiteral("http://192.168.1.77:8899/onvif/ptz"));
}

void testProfilesUrisPresetsAndFaults()
{
    const QByteArray m1Xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <trt:GetProfilesResponse>
      <trt:Profiles token="Profile_1" fixed="true">
        <tt:Name>MainStream</tt:Name>
        <tt:VideoSourceConfiguration token="VSC_1"><tt:Name>VideoSrc</tt:Name></tt:VideoSourceConfiguration>
        <tt:AudioSourceConfiguration token="ASC_1"><tt:Name>MicIn</tt:Name></tt:AudioSourceConfiguration>
        <tt:VideoEncoderConfiguration token="VEC_1">
          <tt:Name>H264_1080p</tt:Name>
          <tt:Encoding>H264</tt:Encoding>
          <tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>
          <tt:RateControl><tt:FrameRateLimit>30</tt:FrameRateLimit></tt:RateControl>
        </tt:VideoEncoderConfiguration>
        <tt:PTZConfiguration token="PTZ_1">
          <tt:NodeToken>PTZNode_1</tt:NodeToken>
        </tt:PTZConfiguration>
      </trt:Profiles>
    </trt:GetProfilesResponse>
  </s:Body>
</s:Envelope>)";

    const QVector<MediaProfile> m1 = OnvifClient::parseProfilesMedia1(m1Xml);
    CHECK(m1.size() == 1);
    if (m1.size() == 1) {
        CHECK(m1.at(0).token == QStringLiteral("Profile_1"));
        CHECK(m1.at(0).name == QStringLiteral("MainStream"));
        CHECK(m1.at(0).videoEncoding == QStringLiteral("H264"));
        CHECK(m1.at(0).width == 1920);
        CHECK(m1.at(0).height == 1080);
        CHECK(m1.at(0).fps == 30.0);
        CHECK(m1.at(0).hasAudio == true);
        CHECK(m1.at(0).hasPtz == true);
        CHECK(m1.at(0).ptzToken == QStringLiteral("PTZNode_1"));
        CHECK(m1.at(0).isMedia2 == false);
    }

    const QByteArray m2Xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tr2="http://www.onvif.org/ver20/media/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tr2:GetProfilesResponse>
      <tr2:Profiles token="Profile_2">
        <tr2:Name>SubStream_HEVC</tr2:Name>
        <tr2:Configurations>
          <tr2:VideoEncoder>
            <tt:Name>HEVC_Enc</tt:Name>
            <tt:Encoding>H265</tt:Encoding>
            <tt:Resolution><tt:Width>1280</tt:Width><tt:Height>720</tt:Height></tt:Resolution>
            <tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit></tt:RateControl>
          </tr2:VideoEncoder>
        </tr2:Configurations>
      </tr2:Profiles>
    </tr2:GetProfilesResponse>
  </s:Body>
</s:Envelope>)";

    const QVector<MediaProfile> m2 = OnvifClient::parseProfilesMedia2(m2Xml);
    CHECK(m2.size() == 1);
    if (m2.size() == 1) {
        CHECK(m2.at(0).token == QStringLiteral("Profile_2"));
        CHECK(m2.at(0).name == QStringLiteral("SubStream_HEVC"));
        CHECK(m2.at(0).videoEncoding == QStringLiteral("H265"));
        CHECK(m2.at(0).width == 1280);
        CHECK(m2.at(0).height == 720);
        CHECK(m2.at(0).fps == 15.0);
        CHECK(m2.at(0).hasAudio == false);
        CHECK(m2.at(0).isMedia2 == true);
    }

    const QByteArray streamUriXml = R"(
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <trt:GetStreamUriResponse>
      <trt:MediaUri><tt:Uri>rtsp://192.168.1.50:554/stream1</tt:Uri></trt:MediaUri>
    </trt:GetStreamUriResponse>
  </s:Body>
</s:Envelope>)";
    CHECK(OnvifClient::parseStreamUri(streamUriXml) == QStringLiteral("rtsp://192.168.1.50:554/stream1"));

    const QByteArray snapUriXml = R"(
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tr2="http://www.onvif.org/ver20/media/wsdl">
  <s:Body>
    <tr2:GetSnapshotUriResponse>
      <tr2:Uri>http://192.168.1.50/onvif-http/snapshot?Profile_1</tr2:Uri>
    </tr2:GetSnapshotUriResponse>
  </s:Body>
</s:Envelope>)";
    CHECK(OnvifClient::parseSnapshotUri(snapUriXml) == QStringLiteral("http://192.168.1.50/onvif-http/snapshot?Profile_1"));

    const QByteArray presetsXml = R"(
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tptz:GetPresetsResponse>
      <tptz:Preset token="1"><tt:Name>Entrada</tt:Name></tptz:Preset>
      <tptz:Preset token="2"><tt:Name>Escritorio</tt:Name></tptz:Preset>
    </tptz:GetPresetsResponse>
  </s:Body>
</s:Envelope>)";
    const QVector<PtzPreset> presets = OnvifClient::parsePresets(presetsXml);
    CHECK(presets.size() == 2);
    if (presets.size() == 2) {
        CHECK(presets.at(0).token == QStringLiteral("1"));
        CHECK(presets.at(0).name == QStringLiteral("Entrada"));
        CHECK(presets.at(1).token == QStringLiteral("2"));
        CHECK(presets.at(1).name == QStringLiteral("Escritorio"));
    }

    const QByteArray soap12AuthFault = R"(<s:Envelope><s:Body><s:Fault><s:Code><s:Subcode><s:Value>ter:NotAuthorized</s:Value></s:Subcode></s:Code></s:Fault></s:Body></s:Envelope>)";
    const QByteArray soap11AuthFault = R"(<SOAP-ENV:Envelope><SOAP-ENV:Body><SOAP-ENV:Fault><faultcode>wsse:FailedAuthentication</faultcode></SOAP-ENV:Fault></SOAP-ENV:Body></SOAP-ENV:Envelope>)";
    const QByteArray nonAuthFault = R"(<s:Envelope><s:Body><s:Fault><s:Code><s:Subcode><s:Value>ter:ActionNotSupported</s:Value></s:Subcode></s:Code></s:Fault></s:Body></s:Envelope>)";

    CHECK(OnvifClient::isAuthFault(QByteArray(), 401));
    CHECK(OnvifClient::isAuthFault(soap12AuthFault, 400));
    CHECK(OnvifClient::isAuthFault(soap11AuthFault, 500));
    CHECK(!OnvifClient::isAuthFault(nonAuthFault, 500));
}

void testPtzStatusLimitsAndKeys()
{
    const QByteArray statusXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tptz:GetStatusResponse>
      <tptz:PTZStatus>
        <tt:Position>
          <tt:PanTilt x="0.325" y="-0.15" space="http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace"/>
          <tt:Zoom x="0.4" space="http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace"/>
        </tt:Position>
        <tt:MoveStatus>
          <tt:PanTilt>MOVING</tt:PanTilt>
          <tt:Zoom>IDLE</tt:Zoom>
        </tt:MoveStatus>
      </tptz:PTZStatus>
    </tptz:GetStatusResponse>
  </s:Body>
</s:Envelope>)";

    const PtzStatus st = OnvifClient::parsePtzStatus(statusXml);
    CHECK(st.valid);
    CHECK(st.position.hasPanTilt);
    CHECK(st.position.hasZoom);
    CHECK(std::abs(st.position.pan - 0.325) < 1e-6);
    CHECK(std::abs(st.position.tilt - (-0.15)) < 1e-6);
    CHECK(std::abs(st.position.zoom - 0.4) < 1e-6);
    CHECK(st.panTiltMoving == true);
    CHECK(st.zoomMoving == false);

    const QByteArray limitsXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tptz:GetConfigurationsResponse>
      <tptz:PTZConfiguration token="PTZCfg_1">
        <tt:PanTiltLimits>
          <tt:Range>
            <tt:XRange><tt:Min>-0.9</tt:Min><tt:Max>0.9</tt:Max></tt:XRange>
            <tt:YRange><tt:Min>-0.6</tt:Min><tt:Max>0.75</tt:Max></tt:YRange>
          </tt:Range>
        </tt:PanTiltLimits>
        <tt:ZoomLimits>
          <tt:Range>
            <tt:XRange><tt:Min>0.0</tt:Min><tt:Max>1.0</tt:Max></tt:XRange>
          </tt:Range>
        </tt:ZoomLimits>
      </tptz:PTZConfiguration>
    </tptz:GetConfigurationsResponse>
  </s:Body>
</s:Envelope>)";

    const PtzLimits lim = OnvifClient::parsePtzLimits(limitsXml);
    CHECK(lim.hasLimits);
    CHECK(std::abs(lim.panMin - (-0.9)) < 1e-6);
    CHECK(std::abs(lim.panMax - 0.9) < 1e-6);
    CHECK(std::abs(lim.tiltMin - (-0.6)) < 1e-6);
    CHECK(std::abs(lim.tiltMax - 0.75) < 1e-6);
    CHECK(std::abs(lim.zoomMin - 0.0) < 1e-6);
    CHECK(std::abs(lim.zoomMax - 1.0) < 1e-6);

    const QString k1 = computeCameraStorageKey(
        QStringLiteral("Polycom"), QStringLiteral("MPTZ-9"), QStringLiteral("SN:AB-123"),
        QStringLiteral("http://192.168.1.96/onvif/device_service"),
        QStringLiteral("http://192.168.1.96/onvif/ptz_service"),
        QStringLiteral("Polycom MPTZ-9"));
    CHECK(k1 == QStringLiteral("serial_polycom_mptz-9_sn_ab-123"));

    const QString kFb = computeCameraStorageKey(
        QStringLiteral("Polycom"), QStringLiteral("MPTZ-9"), QString(),
        QStringLiteral("http://192.168.1.96/onvif/device_service"),
        QStringLiteral("http://192.168.1.96/onvif/ptz_service"),
        QStringLiteral("Polycom MPTZ-9"));
    CHECK(kFb == QStringLiteral("host_192_168_1_96_80_polycom_mptz-9"));
}

void testPanoramaBuilderMapperAndStore()
{
    // Build synthetic textured 960x540 master scene and crop a 3x2 overlapping grid
    // Master size = 960x540, tile size = 480x270, stepX = 240 (50% overlap), stepY = 180 (33% overlap)
    // Wait: 480 + 2*240 = 960; 270 + 180 = 450
    QImage master(960, 450, QImage::Format_RGB32);
    master.fill(QColor(25, 35, 50));
    {
        QPainter painter(&master);
        for (int gx = 0; gx < 960; gx += 48) {
            for (int gy = 0; gy < 450; gy += 45) {
                const int r = (gx * 211 + gy * 37) % 200 + 30;
                const int g = (gx * 53 + gy * 149) % 200 + 30;
                const int b = (gx * 97 + gy * 73) % 200 + 30;
                painter.fillRect(gx + 2, gy + 2, 42, 39, QColor(r, g, b));
                painter.setPen(Qt::white);
                painter.drawLine(gx, gy, gx + 47, gy + 44);
            }
        }
    }

    PanoramaScanSettings scanSettings;
    scanSettings.cols = 3;
    scanSettings.rows = 2;
    scanSettings.panMin = -0.8;
    scanSettings.panMax = 0.8;
    scanSettings.tiltMin = -0.3;
    scanSettings.tiltMax = 0.5;

    QVector<CapturedTile> tiles;
    for (int r = 0; r < 2; ++r) {
        const double tilt = (r == 0) ? 0.5 : -0.3;
        for (int c = 0; c < 3; ++c) {
            const double pan = -0.8 + static_cast<double>(c) * 0.8;
            CapturedTile ct;
            ct.col = c;
            ct.row = r;
            ct.pan = pan;
            ct.tilt = tilt;
            ct.image = master.copy(c * 240, r * 180, 480, 270);
            tiles.push_back(ct);
        }
    }

    const PanoramaBuildResult built = PanoramaBuilder::build(tiles, scanSettings);
    CHECK(built.ok);
    CHECK(!built.image.isNull());
    // Must detect significant overlap -> stitched width strictly less than un-overlapped 3 * 480 = 1440
    CHECK(built.image.width() >= 920 && built.image.width() <= 1010);
    CHECK(built.image.height() >= 415 && built.image.height() <= 490);
    CHECK(built.meta.tiles.size() == 6);

    // Round-trip interior point via PanoramaMapper
    double panOut = 0.0, tiltOut = 0.0;
    const double testPx = 0.37 * built.meta.width;
    const double testPy = 0.42 * built.meta.height;
    CHECK(PanoramaMapper::pixelToPanTilt(built.meta, testPx, testPy, panOut, tiltOut));

    double pxBack = 0.0, pyBack = 0.0;
    CHECK(PanoramaMapper::panTiltToPixel(built.meta, panOut, tiltOut, pxBack, pyBack));
    CHECK(std::abs(pxBack - testPx) < 1.5);
    CHECK(std::abs(pyBack - testPy) < 1.5);

    // Check inverted Pan + Tilt mapping direction
    PanoramaMeta invMeta = built.meta;
    invMeta.invertPan = true;
    invMeta.invertTilt = true;
    for (PanoramaTileMeta &t : invMeta.tiles) {
        t.pan = -t.pan;
        t.tilt = 0.2 - t.tilt;
    }
    invMeta.panMin = -0.8;
    invMeta.panMax = 0.8;
    invMeta.tiltMin = -0.3;
    invMeta.tiltMax = 0.5;
    double invPanLeft = 0.0, invPanRight = 0.0, tmpTilt = 0.0;
    CHECK(PanoramaMapper::pixelToPanTilt(invMeta, invMeta.width * 0.15, invMeta.height * 0.5, invPanLeft, tmpTilt));
    CHECK(PanoramaMapper::pixelToPanTilt(invMeta, invMeta.width * 0.85, invMeta.height * 0.5, invPanRight, tmpTilt));
    CHECK(invPanLeft > invPanRight);

    // Storage round-trip including fallbackHostKey resolution
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    PanoramaStore store(tmpDir.path());

    CameraPtzProfile prof;
    prof.cameraKey = computeCameraStorageKey(
        QString(), QString(), QString(),
        QStringLiteral("http://192.168.1.96/onvif/device_service"),
        QStringLiteral("http://192.168.1.96/onvif/ptz_service"),
        QStringLiteral("Polycom MPTZ-9"));
    prof.cameraName = QStringLiteral("Polycom MPTZ-9");
    prof.invertPan = true;
    CHECK(store.savePanorama(prof, built.image, built.meta));

    SavedPtzPreset sp;
    sp.id = QStringLiteral("preset_test_1");
    sp.name = QStringLiteral("Pizarron");
    sp.hasPosition = true;
    sp.pan = 0.25;
    sp.tilt = 0.10;
    sp.zoom = 0.35;
    sp.thumbnailFile = store.savePresetThumbnail(prof.cameraKey, sp.id, tiles.first().image);
    CHECK(!sp.thumbnailFile.isEmpty());
    prof.savedPresets.push_back(sp);
    CHECK(store.saveProfile(prof));

    CameraPtzProfile loaded;
    QImage loadedPano;
    // Query with a serial key that doesn't exist yet -> should resolve via fallbackHostKey!
    CHECK(store.loadProfile(QStringLiteral("serial_polycom_mptz-9_sn999"),
                            prof.cameraKey, loaded, loadedPano));
    CHECK(loaded.cameraName == QStringLiteral("Polycom MPTZ-9"));
    CHECK(loaded.invertPan == true);
    CHECK(!loadedPano.isNull());
    CHECK(loaded.panorama.isValid());
    CHECK(loaded.savedPresets.size() == 1);
    CHECK(loaded.savedPresets.at(0).name == QStringLiteral("Pizarron"));
    CHECK(!store.loadPresetThumbnail(loaded.cameraKey, loaded.savedPresets.at(0).thumbnailFile).isNull());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);

    testAuth();
    testWsDiscoveryParser();
    testSystemDateAndTimeParser();
    testDeviceInfoServicesCapabilities();
    testProfilesUrisPresetsAndFaults();
    testPtzStatusLimitsAndKeys();
    testPanoramaBuilderMapperAndStore();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d test check(s) FAILED.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All ONVIF core unit tests passed.\n");
    return EXIT_SUCCESS;
}
