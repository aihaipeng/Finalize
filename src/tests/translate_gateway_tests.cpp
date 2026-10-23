#include "text/engines/translate_gateway.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <cstdio>

// The gateway header promises this friend as the offline door to the private
// protocol helpers; defining it here keeps the parsers and signers covered
// without keys or network access.
class TranslateGatewayTestAccess
{
public:
    static int run();
};

int runTranslateGatewayTests()
{
    return TranslateGatewayTestAccess::run();
}

int TranslateGatewayTestAccess::run()
{
    bool pass = true;
    const auto check = [&pass](bool ok, const char *what) {
        std::printf("%s translate-gateway: %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) pass = false;
    };
    using P = TranslateGateway::Provider;

    // batchLines: chunk by 16 items / 5000 chars, reject oversized lines.
    {
        QString error;
        check(TranslateGateway::batchLines({}, &error).isEmpty() && error.isEmpty(),
              "batchLines: no lines, no chunks");
        QStringList lines;
        for (int i = 0; i < 40; ++i) lines << QStringLiteral("line %1").arg(i);
        const QVector<QStringList> chunks = TranslateGateway::batchLines(lines, &error);
        check(error.isEmpty() && chunks.size() == 3 && chunks.at(0).size() == 16
                  && chunks.at(2).size() == 8,
              "batchLines: splits at 16 items per request");
        error.clear();
        const QStringList oversized = { QStringLiteral("a"), QString(5001, QLatin1Char('a')) };
        check(TranslateGateway::batchLines(oversized, &error).isEmpty() && !error.isEmpty(),
              "batchLines: rejects a line over 5000 chars");
    }

    // Language-code mapping and URL shapes for the chinese variants.
    check(TranslateGateway::edgeUrl(QStringLiteral("zh"), QStringLiteral("en"))
                  .contains(QLatin1String("from=zh-Hans&to=en")),
          "edgeUrl maps zh to zh-Hans");
    check(TranslateGateway::googleUrl({}, QStringLiteral("zh-Hant"))
                  .contains(QLatin1String("sl=auto&tl=zh-TW")),
          "googleUrl maps zh-Hant to zh-TW with auto source");

    // Edge reply: array of entries with translations, line count enforced.
    {
        const QByteArray body = QJsonDocument(QJsonArray{
            QJsonObject{{QStringLiteral("translations"),
                         QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("hello")}}}},
                        {QStringLiteral("detectedLanguage"),
                         QJsonObject{{QStringLiteral("language"), QStringLiteral("zh")}}}},
            QJsonObject{{QStringLiteral("translations"),
                         QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("world")}}}}},
        }).toJson(QJsonDocument::Compact);
        const TranslateResult aligned = TranslateGateway::parseEdgeReply(body, 2);
        check(aligned.error.isEmpty()
                  && aligned.lines == (QStringList{QStringLiteral("hello"), QStringLiteral("world")})
                  && aligned.detectedSource == QStringLiteral("zh"),
              "parseEdgeReply aligns lines and detection");
        check(!TranslateGateway::parseEdgeReply(body, 3).error.isEmpty(),
              "parseEdgeReply rejects wrong line counts");
        check(!TranslateGateway::parseEdgeReply(QByteArrayLiteral("{\"a\":1}"), 1).error.isEmpty(),
              "parseEdgeReply rejects non-array bodies");
    }

    // Google reply: array of arrays [text, detected].
    {
        const QByteArray body = QJsonDocument(QJsonArray{
            QJsonArray{QStringLiteral("hello"), QStringLiteral("zh")},
            QJsonArray{QStringLiteral("world"), QStringLiteral("zh")},
        }).toJson(QJsonDocument::Compact);
        const TranslateResult result = TranslateGateway::parseGoogleReply(body, 2);
        check(result.error.isEmpty() && result.lines.size() == 2
                  && result.lines.at(1) == QStringLiteral("world"),
              "parseGoogleReply reads [text, detected] entries");
        check(!TranslateGateway::parseGoogleReply(QByteArrayLiteral("[]"), 1).error.isEmpty(),
              "parseGoogleReply rejects empty arrays");
    }

    // Youdao sign/pointParam: stable vectors pinning the field-order contract.
    {
        const QVector<std::pair<QString, QString>> fields{
            {QStringLiteral("client"), QStringLiteral("webmain")},
            {QStringLiteral("mysticTime"), QStringLiteral("1700000000000")},
            {QStringLiteral("yduuid"), QStringLiteral("uuid-1")},
        };
        check(TranslateGateway::youdaoPointParam(fields)
                      == QStringLiteral("client,mysticTime,yduuid,key"),
              "youdaoPointParam lists field names plus key");
        const QByteArray expect = QCryptographicHash::hash(
                    QByteArrayLiteral("client=webmain&mysticTime=1700000000000&yduuid=uuid-1&key=secret"),
                    QCryptographicHash::Md5);
        check(TranslateGateway::youdaoSign(fields, QStringLiteral("secret"))
                      == QString::fromLatin1(expect.toHex()),
              "youdaoSign md5s the ordered key=value&key= chain");
        check(TranslateGateway::youdaoEncode(QStringLiteral("a b'c")) == QStringLiteral("a%20b'c"),
              "youdaoEncode percent-encodes but keeps !'()*");
    }

    // Youdao SSE reply: transIncre fragments merge, type carries detection.
    {
        const QByteArray body =
            "data:{\"transIncre\":\"ni\",\"type\":\"zh-CHS2en\"}\n"
            "data:{\"transIncre\":\"hao\"}\n";
        const TranslateResult result = TranslateGateway::parseYoudaoReply(body, 1);
        check(result.error.isEmpty()
                  && result.lines == (QStringList{QStringLiteral("nihao")})
                  && result.detectedSource == QStringLiteral("zh"),
              "parseYoudaoReply merges SSE fragments and maps detection");
        const TranslateResult rejected = TranslateGateway::parseYoudaoReply(
                    QByteArrayLiteral("{\"code\":411,\"msg\":\"too long\"}"), 1);
        check(!rejected.error.isEmpty() && rejected.error.contains(QLatin1String("411")),
              "parseYoudaoReply surfaces plain-json rejections");
        check(!TranslateGateway::parseYoudaoReply(QByteArrayLiteral("{oops"), 1).error.isEmpty(),
              "parseYoudaoReply rejects malformed bytes");
    }

    // Sogou: secretCode scrape + reply parsing with CRLF and trailing newline.
    {
        check(TranslateGateway::sogouSecretCode(
                  QByteArrayLiteral("window.__INITIAL_STATE__={\"secretCode\":123456}"))
                  == QStringLiteral("123456"),
              "sogouSecretCode scrapes the rendered code");
        check(TranslateGateway::sogouSecretCode(QByteArrayLiteral("<html></html>")).isEmpty(),
              "sogouSecretCode returns empty without a code");
        const QJsonObject translate{{QStringLiteral("errorCode"), QStringLiteral("0")},
                                    {QStringLiteral("dit"), QStringLiteral("uno\r\ndos\n")}};
        const QJsonObject data{{QStringLiteral("translate"), translate},
                               {QStringLiteral("detect"),
                                QJsonObject{{QStringLiteral("detect"), QStringLiteral("en")}}}};
        const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("data"), data}})
                                    .toJson(QJsonDocument::Compact);
        const TranslateResult result = TranslateGateway::parseSogouReply(body, 2);
        check(result.error.isEmpty()
                  && result.lines == (QStringList{QStringLiteral("uno"), QStringLiteral("dos")}),
              "parseSogouReply normalizes newlines and trims the tail");
        const QJsonObject bad{{QStringLiteral("errorCode"), QStringLiteral("s10")},
                              {QStringLiteral("dit"), QString()}};
        const QByteArray badBody = QJsonDocument(QJsonObject{
                {QStringLiteral("data"), QJsonObject{{QStringLiteral("translate"), bad}}}})
                .toJson(QJsonDocument::Compact);
        check(TranslateGateway::parseSogouReply(badBody, 1).error.contains(QLatin1String("s10")),
              "parseSogouReply reports error codes");
    }

    // Transmart reply: ret_code gate plus aligned auto_translation array.
    {
        const QJsonObject root{
            {QStringLiteral("header"), QJsonObject{{QStringLiteral("ret_code"), QStringLiteral("succ")}}},
            {QStringLiteral("src_lang"), QStringLiteral("zh-TW")},
            {QStringLiteral("auto_translation"), QJsonArray{QStringLiteral("one"), QStringLiteral("two")}}};
        const TranslateResult result = TranslateGateway::parseTransmartReply(
                    QJsonDocument(root).toJson(QJsonDocument::Compact), 2);
        check(result.error.isEmpty() && result.lines.size() == 2
                  && result.detectedSource == QStringLiteral("zh-Hant"),
              "parseTransmartReply maps zh-TW detection back to zh-Hant");
        const QJsonObject failing{
            {QStringLiteral("header"), QJsonObject{{QStringLiteral("ret_code"), QStringLiteral("fail")}}}};
        check(!TranslateGateway::parseTransmartReply(
                  QJsonDocument(failing).toJson(QJsonDocument::Compact), 1).error.isEmpty(),
              "parseTransmartReply fails on non-succ ret_code");
    }

    // Volcengine reply: base_resp status gate and translation join.
    {
        const QJsonObject root{
            {QStringLiteral("base_resp"), QJsonObject{{QStringLiteral("status_code"), 0}}},
            {QStringLiteral("detected_language"), QStringLiteral("en")},
            {QStringLiteral("translation"), QStringLiteral("aa\nbb")}};
        const TranslateResult result = TranslateGateway::parseVolcengineReply(
                    QJsonDocument(root).toJson(QJsonDocument::Compact), 2);
        check(result.error.isEmpty()
                  && result.lines == (QStringList{QStringLiteral("aa"), QStringLiteral("bb")}),
              "parseVolcengineReply splits joined translations");
        const QJsonObject denied{
            {QStringLiteral("base_resp"), QJsonObject{{QStringLiteral("status_code"), 100010}}}};
        check(!TranslateGateway::parseVolcengineReply(
                  QJsonDocument(denied).toJson(QJsonDocument::Compact), 1).error.isEmpty(),
              "parseVolcengineReply surfaces base_resp errors");
    }

    // Deepl oneshot: translations[] with per-line detected language.
    {
        const QJsonObject root{
            {QStringLiteral("translations"), QJsonArray{
                QJsonObject{{QStringLiteral("text"), QStringLiteral("hallo")},
                            {QStringLiteral("detected_source_language"), QStringLiteral("ZH")}},
                QJsonObject{{QStringLiteral("text"), QStringLiteral("welt")},
                            {QStringLiteral("detected_source_language"), QStringLiteral("ZH")}}}}};
        const TranslateResult result = TranslateGateway::parseDeeplReply(
                    QJsonDocument(root).toJson(QJsonDocument::Compact), 2);
        check(result.error.isEmpty() && result.lines.at(0) == QStringLiteral("hallo")
                  && result.detectedSource == QStringLiteral("zh"),
              "parseDeeplReply maps ZH detection to zh");
        check(!TranslateGateway::parseDeeplReply(QByteArrayLiteral("{\"message\":\"quota\"}"), 1)
                   .error.isEmpty(),
              "parseDeeplReply surfaces error messages");
    }

    // Passive breaker: cooled providers drop out unless all are cooling.
    {
        const QVector<P> chain{P::Edge, P::Google, P::Deepl};
        QHash<int, qint64> cooldown;
        cooldown[int(P::Google)] = 1000;
        const QVector<P> filtered = TranslateGateway::filterCooledDown(chain, cooldown, 500);
        check(filtered.size() == 2 && !filtered.contains(P::Google),
              "filterCooledDown drops providers still cooling");
        cooldown[int(P::Edge)] = 2000;
        cooldown[int(P::Deepl)] = 3000;
        check(TranslateGateway::filterCooledDown(chain, cooldown, 500).size() == 3,
              "filterCooledDown keeps the full chain when everything cools");
    }

    return pass ? 0 : 1;
}
