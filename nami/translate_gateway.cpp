#include "translate_gateway.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

namespace {

// Batching shared by all providers: at most sixteen lines and five thousand
// characters per request, results aligned with the input list.
const int kMaxItemsPerRequest = 16;
const int kMaxCharsPerRequest = 5000;

// Edge public translation endpoint: no authentication, JSON string-array body,
// response array aligned with the request. Unofficial and without an SLA.
const char kEdgeUrl[] = "https://edge.microsoft.com/translate/translatetext";

// Google public batch endpoint used by the translate web client family: no
// authentication, one urlencoded q entry per line, response array aligned with
// the input as [["translated","detected"],...]. Same unofficial status as Edge
// and it needs proxy reachability in some regions.
const char kGoogleUrl[] = "https://translate.googleapis.com/translate_a/t";
const char kGoogleUserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";

// Web translation protocol of the dictionary provider: a bootstrap-keyed key
// exchange returns the per-run secretKey/token, then the SSE endpoint streams
// concatenated translation fragments. Keyless, direct in-region, no captcha;
// unofficial and without an SLA like the other web endpoints.
const char kYoudaoKeyUrl[] = "https://dict-trans.youdao.com/translate/key";
const char kYoudaoSseUrl[] = "https://dict-trans.youdao.com/webtranslate/sse";
const char kYoudaoOrigin[] = "https://fanyi.youdao.com";
// Bootstrap key baked into the public frontend bundle; rotates with app.js
// versions, which surfaces as the key endpoint returning no key pair.
const char kYoudaoBootstrapKey[] = "kSy5gtKA4yRUxAVPJPrdYKZ0jBKyd3t1";

// Web translation protocol of the search-engine provider: one page GET per
// run installs the cookies and yields the deployment-wide signing code, then
// one signed JSON POST returns the whole chunk with newlines preserved.
// Keyless, direct in-region; unofficial and without an SLA.
const char kSogouPageUrl[] = "https://fanyi.sogou.com/text";
const char kSogouResultUrl[] = "https://fanyi.sogou.com/api/transpc/text/result";
const char kSogouOrigin[] = "https://fanyi.sogou.com";

void setYoudaoHeaders(QNetworkRequest &request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    request.setRawHeader(QByteArrayLiteral("Origin"), QByteArray(kYoudaoOrigin));
    request.setRawHeader(QByteArrayLiteral("Referer"), QByteArray(kYoudaoOrigin));
}

void setSogouHeaders(QNetworkRequest &request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    request.setRawHeader(QByteArrayLiteral("Origin"), QByteArray(kSogouOrigin));
    request.setRawHeader(QByteArrayLiteral("Referer"), QByteArray(kSogouPageUrl));
}

// Interactive translation web endpoint: public JSON API with native string-array
// input and output, zero-auth, and sub-150ms latency direct in-region.
const char kTransmartUrl[] = "https://transmart.qq.com/api/imt";
const char kTransmartOrigin[] = "https://transmart.qq.com";

void setTransmartHeaders(QNetworkRequest &request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    request.setRawHeader(QByteArrayLiteral("Origin"), QByteArray(kTransmartOrigin));
    request.setRawHeader(QByteArrayLiteral("Referer"), QByteArray(kTransmartOrigin));
}

// Browser extension endpoint: zero-auth endpoint for the official extension.
// Strict trailing slash required (otherwise 307-redirected); verifies Origin.
const char kVolcengineCrxUrl[] = "https://translate.volcengine.com/crx/translate/v1/";
const char kVolcengineCrxOrigin[] = "chrome-extension://klgfhbiooeogdfodpopgppeadghjjemk";

void setVolcengineHeaders(QNetworkRequest &request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    request.setRawHeader(QByteArrayLiteral("Origin"), QByteArray(kVolcengineCrxOrigin));
}

// DeepL official Chrome extension oneshot endpoint: stateless single-shot
// JSON endpoint with native text-array input and translations output.
// Keyless: uses literal string 'Authorization: None' and standard User-Agent.
const char kDeeplOneshotUrl[] = "https://oneshot-free.www.deepl.com/v1/translate";

void setDeeplHeaders(QNetworkRequest &request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("None"));
}

} // namespace

TranslateGateway::TranslateGateway(QObject *parent) : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
    // The search-engine provider rejects signed posts without the cookies the
    // page GET installs, so cookies must persist across requests.
    m_net->setCookieJar(new QNetworkCookieJar(m_net));
    connect(m_net, &QNetworkAccessManager::sslErrors, this,
            [](QNetworkReply *reply, const QList<QSslError> &errors) {
        QStringList messages;
        for (const auto &err : errors)
            messages << err.errorString();
        qWarning("TranslateGateway SSL errors on %s: %s",
                 qPrintable(reply->url().toString()),
                 qPrintable(messages.join(QLatin1String(", "))));
    });
}

TranslateGateway::~TranslateGateway()
{
    abort();
}

QString TranslateGateway::providerName(Provider provider)
{
    switch (provider) {
    case Provider::Auto:
        return QStringLiteral("Auto");
    case Provider::Edge:
        return QStringLiteral("Edge");
    case Provider::Google:
        return QStringLiteral("Google");
    case Provider::Youdao:
        return QStringLiteral("Youdao");
    case Provider::Sogou:
        return QStringLiteral("Sogou");
    case Provider::Transmart:
        return QStringLiteral("Transmart");
    case Provider::Volcengine:
        return QStringLiteral("Volcengine");
    case Provider::Deepl:
        return QStringLiteral("DeepL");
    }
    return QStringLiteral("Edge");
}

TranslateGateway::Provider TranslateGateway::providerFromName(const QString &name, bool *ok)
{
    const QString key = name.toLower();
    Provider provider = Provider::Auto;
    bool found = true;
    if (key == QStringLiteral("auto"))
        provider = Provider::Auto;
    else if (key == QStringLiteral("edge"))
        provider = Provider::Edge;
    else if (key == QStringLiteral("google"))
        provider = Provider::Google;
    else if (key == QStringLiteral("youdao"))
        provider = Provider::Youdao;
    else if (key == QStringLiteral("sogou"))
        provider = Provider::Sogou;
    else if (key == QStringLiteral("transmart"))
        provider = Provider::Transmart;
    else if (key == QStringLiteral("volcengine"))
        provider = Provider::Volcengine;
    else if (key == QStringLiteral("deepl"))
        provider = Provider::Deepl;
    else
        found = false;
    if (ok)
        *ok = found;
    return provider;
}

QVector<TranslateGateway::Provider> TranslateGateway::autoChain(const QString &source,
                                                                const QString &target)
{
    // Fast, in-region providers first; proxy-dependent ones last.
    // shortcut: every attempt can burn the full 15s transfer timeout, so an
    // exhausted Auto run may take ~105s; add per-provider timeouts if that hurts.
    static const Provider order[] = { Provider::Edge, Provider::Transmart, Provider::Youdao,
                                      Provider::Volcengine, Provider::Sogou, Provider::Google,
                                      Provider::Deepl };
    const auto accepts = [](Provider provider, const QString &lang, bool isSource) {
        if (lang.isEmpty())
            return true;
        const QVector<TranslateLanguage> languages = isSource ? supportedSourceLanguages(provider)
                                                              : supportedTargetLanguages(provider);
        for (const TranslateLanguage &language : languages) {
            if (language.code == lang)
                return true;
        }
        return false;
    };
    QVector<Provider> chain;
    for (Provider provider : order) {
        if (accepts(provider, source, true) && accepts(provider, target, false))
            chain.append(provider);
    }
    return chain;
}

QVector<TranslateGateway::Provider> TranslateGateway::filterCooledDown(
    const QVector<Provider> &chain, const QHash<int, qint64> &cooldownUntil, qint64 nowMs)
{
    QVector<Provider> active;
    for (Provider provider : chain) {
        if (cooldownUntil.value(static_cast<int>(provider), 0) <= nowMs)
            active.append(provider);
    }
    // When every provider is cooling down, ignore the breaker: trying a
    // possibly dead engine beats refusing to translate.
    return active.isEmpty() ? chain : active;
}

static const QVector<TranslateLanguage> &allLanguages()
{
    static const QVector<TranslateLanguage> languages = {
        {QStringLiteral("zh"), QCoreApplication::translate("TranslateGateway", "简体中文")},
        {QStringLiteral("zh-Hant"), QCoreApplication::translate("TranslateGateway", "繁体中文")},
        {QStringLiteral("en"), QCoreApplication::translate("TranslateGateway", "英语")},
        {QStringLiteral("ja"), QCoreApplication::translate("TranslateGateway", "日语")},
        {QStringLiteral("ko"), QCoreApplication::translate("TranslateGateway", "韩语")},
        {QStringLiteral("fr"), QCoreApplication::translate("TranslateGateway", "法语")},
        {QStringLiteral("de"), QCoreApplication::translate("TranslateGateway", "德语")},
        {QStringLiteral("es"), QCoreApplication::translate("TranslateGateway", "西班牙语")},
        {QStringLiteral("pt"), QCoreApplication::translate("TranslateGateway", "葡萄牙语")},
        {QStringLiteral("it"), QCoreApplication::translate("TranslateGateway", "意大利语")},
        {QStringLiteral("ru"), QCoreApplication::translate("TranslateGateway", "俄语")},
        {QStringLiteral("nl"), QCoreApplication::translate("TranslateGateway", "荷兰语")},
        {QStringLiteral("id"), QCoreApplication::translate("TranslateGateway", "印尼语")},
        {QStringLiteral("ms"), QCoreApplication::translate("TranslateGateway", "马来语")},
        {QStringLiteral("tr"), QCoreApplication::translate("TranslateGateway", "土耳其语")},
        {QStringLiteral("th"), QCoreApplication::translate("TranslateGateway", "泰语")},
        {QStringLiteral("vi"), QCoreApplication::translate("TranslateGateway", "越南语")},
    };
    return languages;
}

static QVector<TranslateLanguage> filterLanguages(const QStringList &codes)
{
    QVector<TranslateLanguage> result;
    const auto &all = allLanguages();
    for (const QString &code : codes) {
        for (const auto &lang : all) {
            if (lang.code == code) {
                result.append(lang);
                break;
            }
        }
    }
    return result;
}

QVector<TranslateLanguage> TranslateGateway::supportedSourceLanguages(Provider provider)
{
    // Auto-detection is handled by caller as an explicit "auto" entry.
    switch (provider) {
    case Provider::Auto:
    case Provider::Edge:
    case Provider::Google:
    case Provider::Volcengine:
    case Provider::Youdao:
        return allLanguages();
    case Provider::Sogou:
        // Sogou web does not support Dutch (nl), Indonesian (id), Malay (ms).
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("zh-Hant"), QStringLiteral("en"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("fr"),
            QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("pt"),
            QStringLiteral("it"), QStringLiteral("ru"), QStringLiteral("tr"),
            QStringLiteral("th"), QStringLiteral("vi")
        });
    case Provider::Transmart:
        // Verified 2026-10-17: ru and th fail as source languages with
        // 'Required engine is not found', so they stay out of the list.
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("zh-Hant"), QStringLiteral("en"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("fr"),
            QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("pt"),
            QStringLiteral("it"), QStringLiteral("tr"), QStringLiteral("vi")
        });
    case Provider::Deepl:
        // DeepL does not support Thai (th) and Malay (ms); Vietnamese (vi)
        // is supported (verified 2026-10-17).
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("zh-Hant"), QStringLiteral("en"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("fr"),
            QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("pt"),
            QStringLiteral("it"), QStringLiteral("ru"), QStringLiteral("nl"),
            QStringLiteral("id"), QStringLiteral("tr"), QStringLiteral("vi")
        });
    }
    return allLanguages();
}

QVector<TranslateLanguage> TranslateGateway::supportedTargetLanguages(Provider provider)
{
    switch (provider) {
    case Provider::Auto:
    case Provider::Edge:
    case Provider::Google:
    case Provider::Volcengine:
    case Provider::Youdao:
        return allLanguages();
    case Provider::Sogou:
        // Sogou does not translate to Traditional Chinese (zh-Hant) from foreign
        // sources (passes through untranslated), and lacks nl, id, ms.
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("en"), QStringLiteral("ja"),
            QStringLiteral("ko"), QStringLiteral("fr"), QStringLiteral("de"),
            QStringLiteral("es"), QStringLiteral("pt"), QStringLiteral("it"),
            QStringLiteral("ru"), QStringLiteral("tr"), QStringLiteral("th"),
            QStringLiteral("vi")
        });
    case Provider::Transmart:
        // Transmart rejects nl, id, ms with 'Required engine is not found'.
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("zh-Hant"), QStringLiteral("en"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("fr"),
            QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("pt"),
            QStringLiteral("it"), QStringLiteral("ru"), QStringLiteral("tr"),
            QStringLiteral("th"), QStringLiteral("vi")
        });
    case Provider::Deepl:
        // DeepL does not support Thai (th) and Malay (ms); Vietnamese (vi)
        // is supported (verified 2026-10-17).
        return filterLanguages({
            QStringLiteral("zh"), QStringLiteral("zh-Hant"), QStringLiteral("en"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("fr"),
            QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("pt"),
            QStringLiteral("it"), QStringLiteral("ru"), QStringLiteral("nl"),
            QStringLiteral("id"), QStringLiteral("tr"), QStringLiteral("vi")
        });
    }
    return allLanguages();
}

const QVector<TranslateLanguage> &TranslateGateway::targetLanguages()
{
    return allLanguages();
}

QVector<QStringList> TranslateGateway::batchLines(const QStringList &lines, QString *error)
{
    QVector<QStringList> chunks;
    QStringList current;
    int chars = 0;
    for (const QString &line : lines) {
        if (line.size() > kMaxCharsPerRequest) {
            if (error)
                *error = QStringLiteral("single line exceeds the %1 character request limit")
                                 .arg(kMaxCharsPerRequest);
            return {};
        }
        if (!current.isEmpty()
            && (current.size() >= kMaxItemsPerRequest || chars + line.size() > kMaxCharsPerRequest)) {
            chunks.append(current);
            current.clear();
            chars = 0;
        }
        current.append(line);
        chars += line.size();
    }
    if (!current.isEmpty())
        chunks.append(current);
    return chunks;
}

QString TranslateGateway::edgeLanguageCode(const QString &code)
{
    // The shared table uses ISO codes; the Edge endpoint wants BCP-47 for Chinese.
    return code == QStringLiteral("zh") ? QStringLiteral("zh-Hans") : code;
}

QString TranslateGateway::edgeUrl(const QString &source, const QString &target)
{
    QString edgeSource = source;
    if (!edgeSource.isEmpty())
        edgeSource = edgeLanguageCode(edgeSource);
    // An empty from parameter enables automatic source-language detection.
    return QStringLiteral("%1?from=%2&to=%3&isEnterpriseClient=false")
            .arg(QString::fromLatin1(kEdgeUrl), edgeSource, edgeLanguageCode(target));
}

QByteArray TranslateGateway::edgeRequestBody(const QStringList &lines)
{
    return QJsonDocument(QJsonArray::fromStringList(lines)).toJson(QJsonDocument::Compact);
}

TranslateResult TranslateGateway::parseEdgeReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonArray entries = document.array();
    if (entries.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(entries.size())
                               .arg(expectedLines);
        return result;
    }
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        const QJsonArray translations = entry.value(QStringLiteral("translations")).toArray();
        if (translations.isEmpty()) {
            result.error = QStringLiteral("provider returned a line without a translation");
            result.lines.clear();
            return result;
        }
        result.lines.append(translations.at(0).toObject().value(QStringLiteral("text")).toString());
        const QString detected = entry.value(QStringLiteral("detectedLanguage")).toObject()
                                         .value(QStringLiteral("language")).toString();
        if (!detected.isEmpty())
            result.detectedSource = detected;
    }
    return result;
}

QString TranslateGateway::googleLanguageCode(const QString &code)
{
    // The shared table uses ISO codes; the Google endpoint wants regional Chinese.
    if (code == QStringLiteral("zh"))
        return QStringLiteral("zh-CN");
    if (code == QStringLiteral("zh-Hant"))
        return QStringLiteral("zh-TW");
    return code;
}

QString TranslateGateway::googleUrl(const QString &source, const QString &target)
{
    const QString googleSource = source.isEmpty() ? QStringLiteral("auto") : googleLanguageCode(source);
    return QStringLiteral("%1?client=gtx&sl=%2&tl=%3")
            .arg(QString::fromLatin1(kGoogleUrl), googleSource, googleLanguageCode(target));
}

QByteArray TranslateGateway::googleRequestBody(const QStringList &lines)
{
    QByteArray body;
    for (const QString &line : lines) {
        if (!body.isEmpty())
            body += '&';
        body += "q=" + QUrl::toPercentEncoding(line);
    }
    return body;
}

TranslateResult TranslateGateway::parseGoogleReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonArray entries = document.array();
    if (entries.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(entries.size())
                               .arg(expectedLines);
        return result;
    }
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonArray entry = entries.at(i).toArray();
        if (entry.isEmpty()) {
            result.error = QStringLiteral("provider returned a line without a translation");
            result.lines.clear();
            return result;
        }
        result.lines.append(entry.at(0).toString());
        const QString detected = entry.at(1).toString();
        if (!detected.isEmpty())
            result.detectedSource = detected;
    }
    return result;
}

QString TranslateGateway::youdaoLanguageCode(const QString &code)
{
    // The web endpoint uses its own chinese codes instead of ISO tags.
    if (code == QStringLiteral("zh"))
        return QStringLiteral("zh-CHS");
    if (code == QStringLiteral("zh-Hant"))
        return QStringLiteral("zh-CHT");
    return code;
}

QString TranslateGateway::youdaoEncode(const QString &text)
{
    // encodeURIComponent-compatible: additionally leaves ! ' ( ) * unescaped.
    // The provider recomputes the signature over the transported value, so the
    // encoding must match the frontend byte for byte.
    return QString::fromUtf8(QUrl::toPercentEncoding(text, QByteArrayLiteral("!'()*")));
}

QVector<std::pair<QString, QString>> TranslateGateway::youdaoKeyFields(const QString &yduuid, qint64 nowMs)
{
    // Field order is part of the signature contract: JS code-unit (strcmp) order.
    return {
        {QStringLiteral("abtest"), QStringLiteral("0")},
        {QStringLiteral("appVersion"), QStringLiteral("12.0.0")},
        {QStringLiteral("client"), QStringLiteral("webmain")},
        {QStringLiteral("keyfrom"), QStringLiteral("webfanyi.webmain")},
        {QStringLiteral("keyid"), QStringLiteral("translate-webmain-key-getter")},
        {QStringLiteral("mid"), QStringLiteral("1")},
        {QStringLiteral("model"), QStringLiteral("1")},
        {QStringLiteral("mysticTime"), QString::number(nowMs)},
        {QStringLiteral("network"), QStringLiteral("wifi")},
        {QStringLiteral("product"), QStringLiteral("webfanyi")},
        {QStringLiteral("screen"), QStringLiteral("1")},
        {QStringLiteral("targetKeyid"), QStringLiteral("translate-webfanyi-webmain")},
        {QStringLiteral("vendor"), QStringLiteral("web")},
        {QStringLiteral("yduuid"), yduuid},
    };
}

QVector<std::pair<QString, QString>> TranslateGateway::youdaoSseFields(const QStringList &lines,
                                                                      const QString &source,
                                                                      const QString &target,
                                                                      const QString &secretKey,
                                                                      const QString &token,
                                                                      const QString &yduuid, qint64 nowMs)
{
    return {
        {QStringLiteral("appVersion"), QStringLiteral("1")},
        {QStringLiteral("client"), QStringLiteral("webmain")},
        {QStringLiteral("from"),
         source.isEmpty() ? QStringLiteral("auto") : youdaoLanguageCode(source)},
        {QStringLiteral("i"), youdaoEncode(lines.join(QLatin1Char('\n')))},
        {QStringLiteral("imei"), QStringLiteral("1")},
        {QStringLiteral("keyId"), QStringLiteral("translate-webfanyi-webmain")},
        {QStringLiteral("keyfrom"), QStringLiteral("webfanyi.webmain")},
        {QStringLiteral("keyid"), QStringLiteral("translate-webfanyi-webmain")},
        {QStringLiteral("mid"), QStringLiteral("1")},
        {QStringLiteral("model"), QStringLiteral("1")},
        {QStringLiteral("modelName"), QStringLiteral("llmLite")},
        {QStringLiteral("mysticTime"), QString::number(nowMs)},
        {QStringLiteral("network"), QStringLiteral("wifi")},
        {QStringLiteral("product"), QStringLiteral("webfanyi")},
        {QStringLiteral("screen"), QStringLiteral("1")},
        {QStringLiteral("signSecretKey"), secretKey},
        {QStringLiteral("source"), QStringLiteral("webmain")},
        {QStringLiteral("to"), youdaoLanguageCode(target)},
        {QStringLiteral("token"), token},
        {QStringLiteral("useTerm"), QStringLiteral("false")},
        {QStringLiteral("vendor"), QStringLiteral("web")},
        {QStringLiteral("yduuid"), yduuid},
    };
}

QString TranslateGateway::youdaoSign(const QVector<std::pair<QString, QString>> &fields, const QString &key)
{
    QStringList pieces;
    for (const auto &field : fields)
        pieces << field.first + QLatin1Char('=') + field.second;
    pieces << QStringLiteral("key=") + key;
    const QByteArray hash = QCryptographicHash::hash(pieces.join(QLatin1Char('&')).toUtf8(),
                                                     QCryptographicHash::Md5);
    return QString::fromLatin1(hash.toHex());
}

QString TranslateGateway::youdaoPointParam(const QVector<std::pair<QString, QString>> &fields)
{
    QStringList names;
    for (const auto &field : fields)
        names << field.first;
    names << QStringLiteral("key");
    return names.join(QLatin1Char(','));
}

QUrl TranslateGateway::youdaoKeyUrl(const QString &yduuid, qint64 nowMs)
{
    const QVector<std::pair<QString, QString>> fields = youdaoKeyFields(yduuid, nowMs);
    QUrlQuery query;
    for (const auto &field : fields)
        query.addQueryItem(field.first, field.second);
    query.addQueryItem(QStringLiteral("sign"),
                       youdaoSign(fields, QString::fromLatin1(kYoudaoBootstrapKey)));
    query.addQueryItem(QStringLiteral("pointParam"), youdaoPointParam(fields));
    QUrl url(QString::fromLatin1(kYoudaoKeyUrl));
    url.setQuery(query);
    return url;
}

TranslateResult TranslateGateway::parseYoudaoReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QString text;
    QString providerCode, providerMessage;
    for (const QByteArray &rawLine : body.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (!line.startsWith("data:"))
            continue;
        QJsonParseError parseError = {};
        const QJsonDocument document = QJsonDocument::fromJson(line.mid(5), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        text += object.value(QStringLiteral("transIncre")).toString();
        const QString type = object.value(QStringLiteral("type")).toString();
        const int split = type.indexOf(QLatin1Char('2'));
        if (split > 0)
            result.detectedSource = type.left(split);
        if (object.contains(QStringLiteral("code"))
            && object.value(QStringLiteral("code")).toInt() != 0 && providerCode.isEmpty()) {
            providerCode = QString::number(object.value(QStringLiteral("code")).toInt());
            providerMessage = object.value(QStringLiteral("msg")).toString();
        }
    }
    if (text.isEmpty()) {
        // Rejections arrive as a plain json object instead of an event stream.
        QJsonParseError parseError = {};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject object = document.object();
            if (object.contains(QStringLiteral("code"))
                && object.value(QStringLiteral("code")).toInt() != 0) {
                providerCode = QString::number(object.value(QStringLiteral("code")).toInt());
                providerMessage = object.value(QStringLiteral("msg")).toString();
            }
        }
        if (!providerCode.isEmpty())
            result.error = QStringLiteral("provider error %1: %2").arg(providerCode, providerMessage);
        else
            result.error = QStringLiteral("empty translation stream");
        return result;
    }
    // Map the provider chinese codes back onto the shared language table.
    if (result.detectedSource == QStringLiteral("zh-CHS"))
        result.detectedSource = QStringLiteral("zh");
    else if (result.detectedSource == QStringLiteral("zh-CHT"))
        result.detectedSource = QStringLiteral("zh-Hant");
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(lines.size())
                               .arg(expectedLines);
        return result;
    }
    result.lines = lines;
    return result;
}

QString TranslateGateway::sogouLanguageCode(const QString &code)
{
    // The search-engine provider reuses the dictionary provider's chinese
    // codes; the remaining codes pass through unchanged.
    return youdaoLanguageCode(code);
}

QString TranslateGateway::sogouSign(const QString &source, const QString &target, const QString &text,
                                    const QString &secretCode)
{
    // Frontend: k = md5(from + to + text + secretCode), UTF-8 bytes.
    const QByteArray hash = QCryptographicHash::hash(
                QString(source + target + text + secretCode).toUtf8(), QCryptographicHash::Md5);
    return QString::fromLatin1(hash.toHex());
}

QByteArray TranslateGateway::sogouRequestBody(const QStringList &lines, const QString &source,
                                              const QString &target, const QString &secretCode)
{
    const QString text = lines.join(QLatin1Char('\n'));
    const QString from = source.isEmpty() ? QStringLiteral("auto") : sogouLanguageCode(source);
    QJsonObject object;
    object.insert(QStringLiteral("from"), from);
    object.insert(QStringLiteral("to"), sogouLanguageCode(target));
    object.insert(QStringLiteral("text"), text);
    object.insert(QStringLiteral("client"), QStringLiteral("pc"));
    object.insert(QStringLiteral("fr"), QStringLiteral("browser_pc"));
    object.insert(QStringLiteral("needQc"), 1);
    object.insert(QStringLiteral("s"), sogouSign(from, sogouLanguageCode(target), text, secretCode));
    object.insert(QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("exchange"), false);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString TranslateGateway::sogouSecretCode(const QByteArray &pageHtml)
{
    // Server-rendered into __INITIAL_STATE__; deployment-wide constant that
    // rotates with deploys, so every run fetches the page once anyway.
    static const QRegularExpression pattern(QStringLiteral("\"secretCode\":(\\d+)"));
    const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(pageHtml));
    return match.hasMatch() ? match.captured(1) : QString();
}

TranslateResult TranslateGateway::parseSogouReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonObject translate = document.object()
            .value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("translate")).toObject();
    const QString errorCode = translate.value(QStringLiteral("errorCode")).toString();
    if (errorCode != QStringLiteral("0")) {
        // s10 covers missing cookies, a bad signature and a rotated code.
        result.error = QStringLiteral("provider error %1").arg(errorCode.isEmpty()
                        ? QStringLiteral("missing errorCode") : errorCode);
        return result;
    }
    const QString detected = document.object()
            .value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("detect")).toObject()
            .value(QStringLiteral("detect")).toString();
    if (detected == QStringLiteral("zh-CHS"))
        result.detectedSource = QStringLiteral("zh");
    else if (detected == QStringLiteral("zh-CHT"))
        result.detectedSource = QStringLiteral("zh-Hant");
    else
        result.detectedSource = detected;
    QString text = translate.value(QStringLiteral("dit")).toString();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() == expectedLines + 1 && lines.last().isEmpty())
        lines.removeLast();
    if (lines.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(lines.size())
                               .arg(expectedLines);
        return result;
    }
    result.lines = lines;
    return result;
}

QString TranslateGateway::transmartLanguageCode(const QString &code)
{
    if (code == QStringLiteral("zh-Hant"))
        return QStringLiteral("zh-TW");
    return code;
}

QByteArray TranslateGateway::transmartRequestBody(const QStringList &lines, const QString &source,
                                                  const QString &target)
{
    QJsonObject header;
    header.insert(QStringLiteral("fn"), QStringLiteral("auto_translation"));
    header.insert(QStringLiteral("client_key"), QStringLiteral("browser-chrome-111.0.0.0"));

    QJsonArray textArray;
    for (const QString &line : lines)
        textArray.append(line);

    QJsonObject srcObj;
    srcObj.insert(QStringLiteral("lang"), source.isEmpty() ? QStringLiteral("auto")
                                                           : transmartLanguageCode(source));
    srcObj.insert(QStringLiteral("text_list"), textArray);

    QJsonObject tgtObj;
    tgtObj.insert(QStringLiteral("lang"), transmartLanguageCode(target));

    QJsonObject root;
    root.insert(QStringLiteral("header"), header);
    root.insert(QStringLiteral("type"), QStringLiteral("plain"));
    root.insert(QStringLiteral("model_category"), QStringLiteral("normal"));
    root.insert(QStringLiteral("source"), srcObj);
    root.insert(QStringLiteral("target"), tgtObj);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

TranslateResult TranslateGateway::parseTransmartReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonObject root = document.object();
    const QString retCode = root.value(QStringLiteral("header")).toObject()
            .value(QStringLiteral("ret_code")).toString();
    if (retCode != QStringLiteral("succ")) {
        result.error = QStringLiteral("provider error: %1").arg(retCode.isEmpty()
                        ? QStringLiteral("unknown error") : retCode);
        return result;
    }

    const QString detected = root.value(QStringLiteral("src_lang")).toString();
    if (detected == QStringLiteral("zh-TW"))
        result.detectedSource = QStringLiteral("zh-Hant");
    else
        result.detectedSource = detected;

    const QJsonArray autoTrans = root.value(QStringLiteral("auto_translation")).toArray();
    if (autoTrans.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(autoTrans.size())
                               .arg(expectedLines);
        return result;
    }
    for (const auto &val : autoTrans)
        result.lines << val.toString();
    return result;
}

QString TranslateGateway::volcengineLanguageCode(const QString &code)
{
    return code;
}

QByteArray TranslateGateway::volcengineRequestBody(const QStringList &lines, const QString &source,
                                                   const QString &target)
{
    QJsonObject object;
    object.insert(QStringLiteral("source_language"),
                  source.isEmpty() ? QStringLiteral("auto") : volcengineLanguageCode(source));
    object.insert(QStringLiteral("target_language"), volcengineLanguageCode(target));
    object.insert(QStringLiteral("text"), lines.join(QLatin1Char('\n')));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

TranslateResult TranslateGateway::parseVolcengineReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonObject root = document.object();
    const int statusCode = root.value(QStringLiteral("base_resp")).toObject()
            .value(QStringLiteral("status_code")).toInt();
    if (statusCode != 0) {
        const QString msg = root.value(QStringLiteral("base_resp")).toObject()
                .value(QStringLiteral("status_message")).toString();
        result.error = QStringLiteral("provider error %1: %2").arg(statusCode).arg(msg);
        return result;
    }

    result.detectedSource = root.value(QStringLiteral("detected_language")).toString();

    QString text = root.value(QStringLiteral("translation")).toString();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() == expectedLines + 1 && lines.last().isEmpty())
        lines.removeLast();
    if (lines.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(lines.size())
                               .arg(expectedLines);
        return result;
    }
    result.lines = lines;
    return result;
}

QString TranslateGateway::deeplLanguageCode(const QString &code)
{
    if (code == QStringLiteral("zh"))
        return QStringLiteral("zh-Hans");
    if (code == QStringLiteral("zh-Hant"))
        return QStringLiteral("zh-Hant");
    return code.toLower();
}

QByteArray TranslateGateway::deeplRequestBody(const QStringList &lines, const QString &source,
                                              const QString &target)
{
    QJsonObject object;
    QJsonArray textArray;
    for (const QString &line : lines)
        textArray.append(line);
    object.insert(QStringLiteral("text"), textArray);
    object.insert(QStringLiteral("target_lang"), deeplLanguageCode(target));
    if (!source.isEmpty() && source != QStringLiteral("auto"))
        object.insert(QStringLiteral("source_lang"), deeplLanguageCode(source));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

TranslateResult TranslateGateway::parseDeeplReply(const QByteArray &body, int expectedLines)
{
    TranslateResult result;
    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("malformed provider response");
        return result;
    }
    const QJsonObject root = document.object();
    if (root.contains(QStringLiteral("message"))) {
        result.error = QStringLiteral("provider error: %1").arg(root.value(QStringLiteral("message")).toString());
        return result;
    }
    const QJsonArray translations = root.value(QStringLiteral("translations")).toArray();
    if (translations.size() != expectedLines) {
        result.error = QStringLiteral("provider returned %1 of %2 lines")
                               .arg(translations.size())
                               .arg(expectedLines);
        return result;
    }
    for (const auto &item : translations) {
        const QJsonObject obj = item.toObject();
        result.lines << obj.value(QStringLiteral("text")).toString();
        if (result.detectedSource.isEmpty()) {
            const QString detected = obj.value(QStringLiteral("detected_source_language")).toString().toLower();
            if (detected == QStringLiteral("zh-hans") || detected == QStringLiteral("zh"))
                result.detectedSource = QStringLiteral("zh");
            else if (detected == QStringLiteral("zh-hant"))
                result.detectedSource = QStringLiteral("zh-Hant");
            else
                result.detectedSource = detected;
        }
    }
    return result;
}

void TranslateGateway::abort()
{
    if (!m_run)
        return;
    if (m_run->activeReply) {
        m_run->activeReply->abort();
        m_run->activeReply = nullptr;
    }
    TranslateResult aborted;
    aborted.error = QStringLiteral("translation aborted");
    aborted.code = TranslateResult::Error::Cancelled;
    finish(aborted);
}

// Provider::Auto runs walk the chain on Network/Protocol failures only. A
// provider that answered but broke the line contract fails the run directly:
// never pass misaligned lines off as a translation. Transport/protocol
// failures also start a five-minute cooldown so later Auto runs skip the
// provider; the next real request after expiry half-opens it.
static const qint64 kProviderCooldownMs = 5 * 60 * 1000;

bool TranslateGateway::advanceFallback(const TranslateResult &failed)
{
    if (failed.code == TranslateResult::Error::Network
        || failed.code == TranslateResult::Error::Protocol) {
        m_cooldownUntil[static_cast<int>(m_run->provider)] =
            QDateTime::currentMSecsSinceEpoch() + kProviderCooldownMs;
    }
    if (m_run->chain.isEmpty())
        return false;
    const int at = m_run->chain.indexOf(m_run->provider);
    if (at < 0 || at + 1 >= m_run->chain.size())
        return false;
    m_run->provider = m_run->chain.at(at + 1);
    m_run->chunkIndex = 0;
    m_run->translated.clear();
    m_run->detected.clear();
    m_run->youdaoSecretKey.clear();
    m_run->youdaoToken.clear();
    m_run->sogouSecretCode.clear();
    m_run->sogouPageFetched = false;
    return true;
}

void TranslateGateway::translate(const QStringList &lines, const QString &source, const QString &target,
                                 Provider provider,
                                 const std::function<void(const TranslateResult &)> &done)
{
    TranslateResult rejected;
    if (m_run) {
        rejected.error = QStringLiteral("a translation is already running");
    } else if (lines.isEmpty()) {
        if (done)
            done(TranslateResult());
        return;
    } else {
        // SSE providers drop trailing empty lines (they translate visible
        // lines only), which breaks N-in/N-out alignment; strip them here and
        // re-append blank lines to every completed result.
        int trailing = 0;
        while (trailing < lines.size() && lines.at(lines.size() - 1 - trailing).isEmpty())
            ++trailing;
        const QStringList requested = lines.mid(0, lines.size() - trailing);
        if (requested.isEmpty()) { // all-blank input translates to itself
            TranslateResult blank;
            blank.lines = lines;
            if (done)
                done(blank);
            return;
        }
        QString batchError;
        const QVector<QStringList> chunks = batchLines(requested, &batchError);
        if (!batchError.isEmpty()) {
            rejected.error = batchError;
        } else {
            QVector<Provider> chain;
            if (provider == Provider::Auto) {
                chain = autoChain(source, target);
                if (chain.isEmpty()) {
                    rejected.error = QStringLiteral("no provider supports this language pair");
                    rejected.code = TranslateResult::Error::Unsupported;
                } else {
                    chain = filterCooledDown(chain, m_cooldownUntil,
                                             QDateTime::currentMSecsSinceEpoch());
                }
            }
            if (rejected.error.isEmpty()) {
                m_run = new Run;
                m_run->provider = provider == Provider::Auto ? chain.first() : provider;
                m_run->chain = chain;
                m_run->chunks = chunks;
                m_run->source = source;
                m_run->target = target;
                m_run->youdaoUuid = QUuid::createUuid().toString(QUuid::Id128);
                if (trailing > 0) {
                    m_run->done = [done, trailing](const TranslateResult &result) {
                        if (!done)
                            return;
                        TranslateResult padded = result;
                        for (int i = 0; i < trailing; ++i)
                            padded.lines.append(QString());
                        done(padded);
                    };
                } else {
                    m_run->done = done;
                }
                m_run->started = QDateTime::currentMSecsSinceEpoch();
                sendNext();
                return;
            }
        }
    }
    if (done)
        done(rejected);
}

void TranslateGateway::sendNext()
{
    // Blank chunks need no round trip: blank lines translate to themselves,
    // and the LLM-style backend reflows them away, so skipping keeps the
    // line contract intact for every provider.
    while (m_run->chunkIndex < m_run->chunks.size()) {
        const QStringList &pending = m_run->chunks.at(m_run->chunkIndex);
        bool blank = true;
        for (const QString &line : pending) {
            if (!line.trimmed().isEmpty()) {
                blank = false;
                break;
            }
        }
        if (!blank)
            break;
        m_run->translated += pending;
        ++m_run->chunkIndex;
    }
    if (m_run->chunkIndex >= m_run->chunks.size()) {
        TranslateResult result;
        result.lines = m_run->translated;
        result.detectedSource = m_run->detected;
        finish(result);
        return;
    }
    // The web protocol needs one key exchange per run before any chunk request.
    if (m_run->provider == Provider::Youdao && m_run->youdaoToken.isEmpty()) {
        sendYoudaoKeyRequest();
        return;
    }
    // The search-engine provider needs one page GET per run: it installs the
    // cookies the signed post requires and yields the deployment signing code.
    if (m_run->provider == Provider::Sogou && !m_run->sogouPageFetched) {
        sendSogouPageRequest();
        return;
    }
    const QStringList chunk = m_run->chunks.at(m_run->chunkIndex);
    QNetworkRequest request;
    request.setTransferTimeout(15000);
    QByteArray body;
    if (m_run->provider == Provider::Edge) {
        body = edgeRequestBody(chunk);
        request.setUrl(QUrl(edgeUrl(m_run->source, m_run->target)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    } else if (m_run->provider == Provider::Google) {
        body = googleRequestBody(chunk);
        request.setUrl(QUrl(googleUrl(m_run->source, m_run->target)));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArray(kGoogleUserAgent));
    } else if (m_run->provider == Provider::Youdao) {
        request.setUrl(QUrl(QString::fromLatin1(kYoudaoSseUrl)));
        setYoudaoHeaders(request);
        const QVector<std::pair<QString, QString>> fields = youdaoSseFields(
                    chunk, m_run->source, m_run->target, m_run->youdaoSecretKey, m_run->youdaoToken,
                    m_run->youdaoUuid, QDateTime::currentMSecsSinceEpoch());
        auto *parts = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        const auto addField = [parts](const QString &name, const QString &value) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"%1\"").arg(name));
            part.setBody(value.toUtf8());
            parts->append(part);
        };
        for (const auto &field : fields)
            addField(field.first, field.second);
        addField(QStringLiteral("sign"), youdaoSign(fields, m_run->youdaoSecretKey));
        addField(QStringLiteral("pointParam"), youdaoPointParam(fields));
        QNetworkReply *reply = m_net->post(request, parts);
        parts->setParent(reply);
        m_run->activeReply = reply;
        handleChunkReply(reply);
        return;
    } else if (m_run->provider == Provider::Sogou) {
        body = sogouRequestBody(chunk, m_run->source, m_run->target, m_run->sogouSecretCode);
        request.setUrl(QUrl(QString::fromLatin1(kSogouResultUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json;charset=UTF-8"));
        setSogouHeaders(request);
    } else if (m_run->provider == Provider::Transmart) {
        body = transmartRequestBody(chunk, m_run->source, m_run->target);
        request.setUrl(QUrl(QString::fromLatin1(kTransmartUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        setTransmartHeaders(request);
    } else if (m_run->provider == Provider::Volcengine) {
        body = volcengineRequestBody(chunk, m_run->source, m_run->target);
        request.setUrl(QUrl(QString::fromLatin1(kVolcengineCrxUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        setVolcengineHeaders(request);
    } else if (m_run->provider == Provider::Deepl) {
        body = deeplRequestBody(chunk, m_run->source, m_run->target);
        request.setUrl(QUrl(QString::fromLatin1(kDeeplOneshotUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        setDeeplHeaders(request);
    }
    QNetworkReply *reply = m_net->post(request, body);
    m_run->activeReply = reply;
    handleChunkReply(reply);
}

void TranslateGateway::sendSogouPageRequest()
{
    QNetworkRequest request;
    request.setTransferTimeout(15000);
    request.setUrl(QUrl(QString::fromLatin1(kSogouPageUrl)));
    setSogouHeaders(request);
    QNetworkReply *reply = m_net->get(request);
    m_run->activeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (!m_run || m_run->activeReply != reply)
            return;
        m_run->activeReply = nullptr;
        TranslateResult result;
        if (reply->error() != QNetworkReply::NoError) {
            result.error = QStringLiteral("network error: %1").arg(reply->errorString());
            result.code = TranslateResult::Error::Network;
            if (advanceFallback(result)) {
                sendNext();
                return;
            }
            finish(result);
            return;
        }
        const QByteArray data = reply->readAll();
        m_run->sogouSecretCode = sogouSecretCode(data);
        m_run->sogouPageFetched = true;
        if (m_run->sogouSecretCode.isEmpty()) {
            result.error = QStringLiteral("text page gave no secretCode");
            result.code = TranslateResult::Error::Protocol;
            if (advanceFallback(result)) {
                sendNext();
                return;
            }
            finish(result);
            return;
        }
        sendNext();
    });
}

void TranslateGateway::sendYoudaoKeyRequest()
{
    QNetworkRequest request;
    request.setTransferTimeout(15000);
    request.setUrl(youdaoKeyUrl(m_run->youdaoUuid, QDateTime::currentMSecsSinceEpoch()));
    setYoudaoHeaders(request);
    QNetworkReply *reply = m_net->post(request, QByteArray());
    m_run->activeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (!m_run || m_run->activeReply != reply)
            return;
        m_run->activeReply = nullptr;
        TranslateResult result;
        if (reply->error() != QNetworkReply::NoError) {
            result.error = QStringLiteral("network error: %1").arg(reply->errorString());
            result.code = TranslateResult::Error::Network;
            if (advanceFallback(result)) {
                sendNext();
                return;
            }
            finish(result);
            return;
        }
        const QByteArray data = reply->readAll();
        QJsonParseError parseError = {};
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        const QJsonObject payload = document.object().value(QStringLiteral("data")).toObject();
        m_run->youdaoSecretKey = payload.value(QStringLiteral("secretKey")).toString();
        m_run->youdaoToken = payload.value(QStringLiteral("token")).toString();
        if (m_run->youdaoSecretKey.isEmpty() || m_run->youdaoToken.isEmpty()) {
            // Fixed failure signature of a rotated frontend bootstrap key.
            result.error = QStringLiteral("translate/key gave no secretKey/token");
            result.code = TranslateResult::Error::Protocol;
            if (advanceFallback(result)) {
                sendNext();
                return;
            }
            finish(result);
            return;
        }
        sendNext();
    });
}

void TranslateGateway::handleChunkReply(QNetworkReply *reply)
{
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (!m_run || m_run->activeReply != reply)
            return;
        m_run->activeReply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            TranslateResult result;
            result.error = QStringLiteral("network error: %1").arg(reply->errorString());
            result.code = TranslateResult::Error::Network;
            if (advanceFallback(result)) {
                sendNext();
                return;
            }
            finish(result);
            return;
        }
        const QByteArray data = reply->readAll();
        const int expected = m_run->chunks.at(m_run->chunkIndex).size();
        TranslateResult parsed;
        if (m_run->provider == Provider::Edge)
            parsed = parseEdgeReply(data, expected);
        else if (m_run->provider == Provider::Google)
            parsed = parseGoogleReply(data, expected);
        else if (m_run->provider == Provider::Sogou)
            parsed = parseSogouReply(data, expected);
        else if (m_run->provider == Provider::Transmart)
            parsed = parseTransmartReply(data, expected);
        else if (m_run->provider == Provider::Volcengine)
            parsed = parseVolcengineReply(data, expected);
        else if (m_run->provider == Provider::Deepl)
            parsed = parseDeeplReply(data, expected);
        else
            parsed = parseYoudaoReply(data, expected);
        if (!parsed.error.isEmpty()) {
            // The LLM backend reflows folded lines into one paragraph, which
            // breaks N-in/N-out alignment; retry the same chunk line by line
            // before failing over (single-line chunks cannot be split again,
            // so this cannot loop).
            if (m_run->provider == Provider::Youdao
                    && m_run->chunks.at(m_run->chunkIndex).size() > 1) {
                const QStringList chunk = m_run->chunks.at(m_run->chunkIndex);
                m_run->chunks.remove(m_run->chunkIndex);
                for (int i = chunk.size() - 1; i >= 0; --i)
                    m_run->chunks.insert(m_run->chunkIndex, QStringList{chunk.at(i)});
                sendNext();
                return;
            }
            parsed.code = TranslateResult::Error::Protocol;
            if (advanceFallback(parsed)) {
                sendNext();
                return;
            }
            finish(parsed);
            return;
        }
        m_run->translated += parsed.lines;
        if (!parsed.detectedSource.isEmpty())
            m_run->detected = parsed.detectedSource;
        if (++m_run->chunkIndex >= m_run->chunks.size()) {
            TranslateResult result;
            result.lines = m_run->translated;
            result.detectedSource = m_run->detected;
            finish(result);
        } else {
            sendNext();
        }
    });
}

void TranslateGateway::finish(const TranslateResult &result)
{
    if (!m_run)
        return;
    if (m_run->activeReply) {
        m_run->activeReply->disconnect(this);
        m_run->activeReply = nullptr;
    }
    const std::function<void(const TranslateResult &)> done = m_run->done;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_run->started;
    const QString provider = providerName(m_run->provider);
    delete m_run;
    m_run = nullptr;
    if (done) {
        TranslateResult delivered = result;
        delivered.elapsed = elapsed;
        delivered.provider = provider;
        done(delivered);
    }
}
