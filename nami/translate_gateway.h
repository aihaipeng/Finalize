#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <functional>
#include <utility>

class QNetworkReply;

struct TranslateResult {
    // Failure class of a run. Only Network/Protocol failures may be retried on
    // another provider (Auto routing); the rest terminate the run immediately.
    enum class Error { None, Network, Protocol, Unsupported, Cancelled, Busy };
    QStringList lines;        // One translation per request line, same order.
    QString detectedSource;   // Provider-detected source code when source was auto.
    QString provider;         // Name of the provider that produced this result.
    Error code = Error::None; // Error class; error carries the human message.
    QString error;            // Non-empty on failure; lines is meaningless then.
    qint64 elapsed = 0;
};

struct TranslateLanguage {
    QString code;
    QString name;
};

// Cloud text translation kernel. Providers share one line-based contract
// (lines in, aligned translations out) so results map back onto OCR text
// boxes. Only depends on QtCore/QtNetwork; one instance runs one translation
// at a time - create one instance per concurrent caller.
class TranslateGateway : public QObject
{
    Q_OBJECT
public:
    // Keyless providers plus Auto routing: Auto walks the chain returned by
    // autoChain() and retries the next provider on transport/protocol
    // failures. Edge is the default public endpoint, Google needs proxy
    // reachability in some regions, Youdao uses a two-step signed web
    // protocol, Sogou signs chunks with an SSR code, Transmart takes a native
    // string array with client_key, Volcengine uses the browser extension CRX
    // endpoint, and Deepl uses the official extension oneshot endpoint with
    // literal 'Authorization: None'.
    enum class Provider { Auto, Edge, Google, Youdao, Sogou, Transmart, Volcengine, Deepl };

    explicit TranslateGateway(QObject *parent = nullptr);
    ~TranslateGateway() override; // Aborts an in-flight run.

    static const QVector<TranslateLanguage> &targetLanguages();
    static QVector<TranslateLanguage> supportedSourceLanguages(Provider provider);
    static QVector<TranslateLanguage> supportedTargetLanguages(Provider provider);
    static QString providerName(Provider provider);
    // Case-insensitive lookup ("auto", "edge", ... "deepl"); ok is false on
    // unknown names, which then return Provider::Auto.
    static Provider providerFromName(const QString &name, bool *ok = nullptr);
    // Routing order for Provider::Auto, filtered to providers supporting the
    // language pair. An empty source code means auto-detect (all accept).
    static QVector<Provider> autoChain(const QString &source, const QString &target);
    // Passive breaker: drop providers whose cooldown (started by a
    // Network/Protocol failure) is still running at nowMs. Returns the full
    // chain when all are cooling: trying a dead engine beats refusing.
    static QVector<Provider> filterCooledDown(const QVector<Provider> &chain,
                                              const QHash<int, qint64> &cooldownUntil,
                                              qint64 nowMs);

    bool busy() const { return m_run != nullptr; }
    void abort();

    // The one entry point: lines in, aligned lines out. An empty source means
    // auto-detection; done fires on the event loop, never reentrantly.
    void translate(const QStringList &lines, const QString &source, const QString &target,
                   Provider provider,
                   const std::function<void(const TranslateResult &)> &done);

private:
    // The regression suite reaches the protocol helpers below through this
    // friend; they are implementation details, not part of the published API.
    friend class TranslateGatewayTestAccess;

    // Pure helpers, offline-testable without keys or network.
    static QVector<QStringList> batchLines(const QStringList &lines, QString *error);
    static QString edgeLanguageCode(const QString &code);
    static QString edgeUrl(const QString &source, const QString &target);
    static QByteArray edgeRequestBody(const QStringList &lines);
    static TranslateResult parseEdgeReply(const QByteArray &body, int expectedLines);
    static QString googleLanguageCode(const QString &code);
    static QString googleUrl(const QString &source, const QString &target);
    static QByteArray googleRequestBody(const QStringList &lines);
    static TranslateResult parseGoogleReply(const QByteArray &body, int expectedLines);
    static QString youdaoLanguageCode(const QString &code);
    static QString youdaoEncode(const QString &text);
    static QVector<std::pair<QString, QString>> youdaoKeyFields(const QString &yduuid, qint64 nowMs);
    static QVector<std::pair<QString, QString>> youdaoSseFields(const QStringList &lines,
                                                                const QString &source,
                                                                const QString &target,
                                                                const QString &secretKey,
                                                                const QString &token, const QString &yduuid,
                                                                qint64 nowMs);
    static QString youdaoSign(const QVector<std::pair<QString, QString>> &fields, const QString &key);
    static QString youdaoPointParam(const QVector<std::pair<QString, QString>> &fields);
    static QUrl youdaoKeyUrl(const QString &yduuid, qint64 nowMs);
    static TranslateResult parseYoudaoReply(const QByteArray &body, int expectedLines);
    static QString sogouLanguageCode(const QString &code);
    static QString sogouSign(const QString &source, const QString &target, const QString &text,
                             const QString &secretCode);
    static QByteArray sogouRequestBody(const QStringList &lines, const QString &source,
                                       const QString &target, const QString &secretCode);
    static QString sogouSecretCode(const QByteArray &pageHtml);
    static TranslateResult parseSogouReply(const QByteArray &body, int expectedLines);
    static QString transmartLanguageCode(const QString &code);
    static QByteArray transmartRequestBody(const QStringList &lines, const QString &source,
                                           const QString &target);
    static TranslateResult parseTransmartReply(const QByteArray &body, int expectedLines);
    static QString volcengineLanguageCode(const QString &code);
    static QByteArray volcengineRequestBody(const QStringList &lines, const QString &source,
                                            const QString &target);
    static TranslateResult parseVolcengineReply(const QByteArray &body, int expectedLines);
    static QString deeplLanguageCode(const QString &code);
    static QByteArray deeplRequestBody(const QStringList &lines, const QString &source,
                                       const QString &target);
    static TranslateResult parseDeeplReply(const QByteArray &body, int expectedLines);

    void sendNext();
    void sendYoudaoKeyRequest();
    void sendSogouPageRequest();
    bool advanceFallback(const TranslateResult &failed);
    void handleChunkReply(QNetworkReply *reply);
    void finish(const TranslateResult &result);

    struct Run {
        Provider provider = Provider::Edge;
        QVector<Provider> chain;  // Auto routing order; empty for direct runs.
        QVector<QStringList> chunks;
        int chunkIndex = 0;
        QStringList translated;
        QString source, target, detected;
        QString youdaoUuid, youdaoSecretKey, youdaoToken;
        QString sogouSecretCode;
        bool sogouPageFetched = false;
        QPointer<QNetworkReply> activeReply;
        std::function<void(const TranslateResult &)> done;
        qint64 started = 0;
    };
    class QNetworkAccessManager *m_net = nullptr;
    Run *m_run = nullptr;
    QHash<int, qint64> m_cooldownUntil; // Passive breaker: provider -> epoch ms.
};
