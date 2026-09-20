#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QObject>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSet>
#include <QSysInfo>
#include <QSvgRenderer>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QWindow>
#include <QStringConverter>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QtQml/qqml.h>
#include <QtGlobal>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "xovi.h"

namespace {

QMutex g_inputMutex;
QQmlEngine *g_engine = nullptr;
QObject *g_helper = nullptr;

QImage loadTransferImage(const QString &path)
{
    if (!path.endsWith(".svg", Qt::CaseInsensitive))
        return QImage(path);

    QSvgRenderer renderer(path);
    if (!renderer.isValid())
        return {};

    QSize size = renderer.defaultSize();
    if (!size.isValid() || size.isEmpty())
        size = {1024, 1024};
    size.scale(2048, 2048, Qt::KeepAspectRatio);

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}

QJsonObject failure(const QString &code, const QString &message)
{
    return {
        {"ok", false},
        {"error", code},
        {"message", message},
    };
}

char *response(const QJsonObject &object)
{
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return ::strdup(data.constData());
}

bool runOnGuiSync(const std::function<void()> &job)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return false;
    if (QThread::currentThread() == app->thread()) {
        job();
        return true;
    }
    return QMetaObject::invokeMethod(app, job, Qt::BlockingQueuedConnection);
}

QQmlEngine *findEngine()
{
    if (g_engine)
        return g_engine;
    for (QWindow *window : QGuiApplication::allWindows()) {
        if (QQmlEngine *engine = qmlEngine(window)) {
            g_engine = engine;
            return engine;
        }
    }
    return nullptr;
}

QString describeQmlErrors(const QQmlComponent &component)
{
    QStringList errors;
    for (const QQmlError &error : component.errors())
        errors.push_back(error.toString());
    return errors.join("; ");
}

QObject *ensureHelper(QString *error)
{
    if (g_helper)
        return g_helper;

    QQmlEngine *engine = findEngine();
    if (!engine) {
        if (error)
            *error = "no QML engine is available";
        return nullptr;
    }

    static const char helperQml[] = R"QML(
import QtQml
import com.remarkable

QtObject {
    property var sceneController: null

    function insertText(value, x, y, coordinateSpace) {
        const controller = sceneController
        if (!controller)
            return "controller-not-found"

        try {
            let position = Qt.point(x, y)
            if (coordinateSpace === "normalized") {
                let bounds = controller.paperNoteBounds
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    bounds = controller.defaultNoteBounds
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    bounds = controller.boundingRect
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    return "page-bounds-unavailable"
                position = Qt.point(bounds.x + x * bounds.width,
                                    bounds.y + y * bounds.height)
            }

            Clipboard.setTextFromString(value)
            if (!Clipboard.hasText)
                return "clipboard-rejected-text"

            if (!controller.hasRootDocument)
                controller.createRootDocument(ParagraphStyle.Type.Title)
            controller.focusRootDocument(position)
            controller.pasteText(Clipboard.text, SceneController.KeepStyle, 0)
            return "ok"
        } catch (exception) {
            return "exception:" + exception
        }
    }

    function insertImageFile(fileUrl, x, y, coordinateSpace) {
        const controller = sceneController
        if (!controller)
            return "controller-not-found"

        try {
            let position = Qt.point(x, y)
            if (coordinateSpace === "normalized") {
                let bounds = controller.paperNoteBounds
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    bounds = controller.defaultNoteBounds
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    bounds = controller.boundingRect
                if (!bounds || bounds.width <= 0 || bounds.height <= 0)
                    return "page-bounds-unavailable"
                position = Qt.point(bounds.x + x * bounds.width,
                                    bounds.y + y * bounds.height)
            }
            controller.insertImageFileAsSceneItem(fileUrl, position)
            return "ok"
        } catch (exception) {
            return "exception:" + exception
        }
    }
}
)QML";

    QQmlComponent component(engine);
    component.setData(helperQml, QUrl(QStringLiteral("xovi-content-inserter-helper.qml")));
    if (component.isError()) {
        if (error)
            *error = describeQmlErrors(component);
        return nullptr;
    }

    g_helper = component.create();
    if (!g_helper && error)
        *error = describeQmlErrors(component);
    return g_helper;
}

bool isSceneController(QObject *object)
{
    if (!object || !object->metaObject())
        return false;
    return QByteArray(object->metaObject()->className()).contains("SceneController");
}

QObject *controllerProperty(QObject *object, const char *name)
{
    if (!object)
        return nullptr;
    const QVariant value = object->property(name);
    QObject *candidate = value.value<QObject *>();
    return isSceneController(candidate) ? candidate : nullptr;
}

QObject *objectProperty(QObject *object, const char *name)
{
    return object ? object->property(name).value<QObject *>() : nullptr;
}

QList<QObject *> runtimeObjects()
{
    QList<QObject *> objects;
    QList<QObject *> pending;
    QSet<QObject *> seen;

    if (QCoreApplication *app = QCoreApplication::instance())
        pending.push_back(app);
    for (QWindow *window : QGuiApplication::allWindows()) {
        pending.push_back(window);
        if (QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window))
            pending.push_back(quickWindow->contentItem());
    }
    if (QQmlEngine *engine = findEngine()) {
        pending.push_back(engine);
        if (QQmlApplicationEngine *applicationEngine = qobject_cast<QQmlApplicationEngine *>(engine))
            pending.append(applicationEngine->rootObjects());
    }

    while (!pending.isEmpty()) {
        QObject *object = pending.takeLast();
        if (!object || seen.contains(object))
            continue;
        seen.insert(object);
        objects.push_back(object);
        pending.append(object->children());
        if (QQuickItem *item = qobject_cast<QQuickItem *>(object)) {
            // Loader-created objects can have only a visual parent. In that
            // case they are absent from QObject::children(), but remain in the
            // QQuickItem tree rooted at QQuickWindow::contentItem().
            for (QQuickItem *child : item->childItems())
                pending.push_back(child);
        }
    }
    return objects;
}

bool isVisibleObject(QObject *object)
{
    if (QQuickItem *item = qobject_cast<QQuickItem *>(object))
        return item->isVisible();
    if (QWindow *window = qobject_cast<QWindow *>(object))
        return window->isVisible();
    return true;
}

struct ActivePageContext {
    QObject *documentView = nullptr;
    QObject *controller = nullptr;
    QString pageId;
};

ActivePageContext contextFromDocumentView(QObject *documentView)
{
    if (!documentView || !isVisibleObject(documentView) ||
        !documentView->property("documentLoaded").toBool()) {
        return {};
    }
    QObject *controller = controllerProperty(documentView, "sceneController");
    if (!controller)
        return {};
    return {documentView, controller, documentView->property("currentPageId").toString()};
}

bool hasProperty(QObject *object, const char *name)
{
    return object && object->metaObject() && object->metaObject()->indexOfProperty(name) >= 0;
}

void logPageCandidates(const QList<QObject *> &objects)
{
    qInfo("[xovi-content-inserter] page detection scanned %lld runtime objects",
          static_cast<long long>(objects.size()));
    int logged = 0;
    for (QObject *object : objects) {
        if (!object || !object->metaObject())
            continue;
        const QByteArray className = object->metaObject()->className();
        const bool candidate = object->objectName() == QStringLiteral("DocumentView") ||
            className.contains("DocumentView") || className.contains("DeviceSceneView") ||
            hasProperty(object, "sceneController");
        if (!candidate || logged++ >= 20)
            continue;

        QObject *loadedItem = objectProperty(object, "item");
        QObject *sceneController = objectProperty(object, "sceneController");
        QObject *controller = objectProperty(object, "controller");
        qInfo("[xovi-content-inserter] page candidate class=%s name=%s visible=%d "
              "documentLoaded=%d item=%p itemClass=%s sceneController=%p controller=%p",
            className.constData(), qPrintable(object->objectName()), isVisibleObject(object),
            object->property("documentLoaded").toBool(), loadedItem,
            loadedItem && loadedItem->metaObject() ? loadedItem->metaObject()->className() : "-",
            sceneController, controller);
    }
}

ActivePageContext findActivePage()
{
    const QList<QObject *> objects = runtimeObjects();

    // MainView gives its active asynchronous loader a stable objectName. Its
    // QML class remains QQuickLoader, so filtering on a DocumentView class name
    // misses it on 3.27/3.28.
    for (QObject *object : objects) {
        if (object->objectName() != QStringLiteral("DocumentView") || !isVisibleObject(object))
            continue;
        if (ActivePageContext context = contextFromDocumentView(objectProperty(object, "item"));
            context.controller) {
            qInfo("[xovi-content-inserter] active page found through DocumentView loader pageId=%s",
                qPrintable(context.pageId));
            return context;
        }
    }

    // Also accept the loaded DocumentView item directly. QQmlEngine root
    // objects and visual parents are not guaranteed to mirror QObject parentage.
    for (QObject *object : objects) {
        if (!object->metaObject())
            continue;
        const QByteArray className = object->metaObject()->className();
        if (className.contains("DocumentView")) {
            if (ActivePageContext context = contextFromDocumentView(object); context.controller) {
                qInfo("[xovi-content-inserter] active page found directly pageId=%s",
                    qPrintable(context.pageId));
                return context;
            }
        }
        if (className.contains("DeviceSceneView") && isVisibleObject(object)) {
            if (QObject *controller = controllerProperty(object, "controller")) {
                qInfo("[xovi-content-inserter] active page found through DeviceSceneView pageId=%s",
                    qPrintable(object->property("pageId").toString()));
                return {nullptr, controller, object->property("pageId").toString()};
            }
        }
    }

    // Do not fall back to an arbitrary controller: xochitl keeps controllers
    // for hidden previews and previously opened pages alive. Input injection
    // against one of those while the library is visible would draw elsewhere
    // on the screen instead of into a notebook.
    logPageCandidates(objects);
    return {};
}

struct TextPayload {
    QString text;
    QString path;
    QString errorCode;
    QString errorMessage;

    bool isValid() const { return errorCode.isEmpty(); }
};

TextPayload loadTextPayload(const QJsonObject &request)
{
    const bool hasInlineText = request.contains("text");
    const bool hasPath = request.contains("path");
    if (hasInlineText == hasPath) {
        return {{}, {}, "invalid-text-source", "text requests require exactly one of text or path"};
    }

    TextPayload payload;
    if (hasInlineText) {
        if (!request.value("text").isString())
            return {{}, {}, "invalid-text", "text must be a JSON string"};
        payload.text = request.value("text").toString();
    } else {
        if (!request.value("path").isString() || request.value("path").toString().isEmpty())
            return {{}, {}, "invalid-text-path", "path must be a non-empty string"};
        payload.path = request.value("path").toString();
        QFile file(payload.path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {{}, payload.path, "text-file-open-failed",
                    QStringLiteral("cannot open %1: %2").arg(payload.path, file.errorString())};
        }
        QByteArray bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            return {{}, payload.path, "text-file-read-failed",
                    QStringLiteral("cannot read %1: %2").arg(payload.path, file.errorString())};
        }
        if (bytes.startsWith("\xEF\xBB\xBF"))
            bytes.remove(0, 3);
        QStringDecoder decoder(QStringDecoder::Utf8);
        payload.text = decoder.decode(bytes);
        if (decoder.hasError()) {
            return {{}, payload.path, "invalid-text-encoding",
                    QStringLiteral("%1 is not valid UTF-8 text").arg(payload.path)};
        }
    }

    if (payload.text.isEmpty())
        return {{}, payload.path, "empty-text", "text must not be empty"};
    return payload;
}

QJsonObject insertSceneImage(const QJsonObject &request)
{
    const QString path = request.value("path").toString();
    if (path.isEmpty())
        return failure("missing-path", "image requests require a device-local path");
    const QImage image(path);
    if (image.isNull())
        return failure("image-load-failed", QStringLiteral("cannot decode image: ") + path);

    const QString coordinateSpace = request.value("coordinateSpace").toString("normalized");
    if (coordinateSpace != "normalized" && coordinateSpace != "scene")
        return failure("invalid-coordinate-space", "scene-image insertion supports normalized or scene coordinates");
    const double x = request.value("x").toDouble(0.5);
    const double y = request.value("y").toDouble(0.5);
    if (coordinateSpace == "normalized" && (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0))
        return failure("coordinate-out-of-range", "normalized x and y must be within [0,1]");

    QString operationResult;
    QString helperError;
    QString pageId;
    const bool dispatched = runOnGuiSync([&]() {
        const ActivePageContext context = findActivePage();
        if (!context.controller) {
            operationResult = "controller-not-found";
            return;
        }
        pageId = context.pageId;
        QObject *helper = ensureHelper(&helperError);
        if (!helper) {
            operationResult = "helper-unavailable";
            return;
        }
        helper->setProperty("sceneController", QVariant::fromValue(context.controller));
        QVariant result;
        if (!QMetaObject::invokeMethod(
                helper,
                "insertImageFile",
                Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, QVariant(QUrl::fromLocalFile(path))),
                Q_ARG(QVariant, QVariant(x)),
                Q_ARG(QVariant, QVariant(y)),
                Q_ARG(QVariant, QVariant(coordinateSpace)))) {
            operationResult = "invoke-failed";
            return;
        }
        operationResult = result.toString();
    });

    if (!dispatched)
        return failure("gui-unavailable", "cannot dispatch image insertion to the GUI thread");
    if (operationResult != "ok") {
        QString detail = operationResult;
        if (!helperError.isEmpty())
            detail += QStringLiteral(": ") + helperError;
        return failure("image-insert-failed", detail);
    }

    return {
        {"ok", true},
        {"target", "current-page"},
        {"type", "image"},
        {"pageId", pageId},
        {"path", path},
        {"representation", "scene-image"},
        {"coordinateSpace", coordinateSpace},
        {"x", x},
        {"y", y},
        {"sourceWidth", image.width()},
        {"sourceHeight", image.height()},
        {"warning", "the native image item is inserted asynchronously and can be resized with xochitl's selection tool"},
    };
}

QJsonObject insertText(const QJsonObject &request)
{
    const TextPayload payload = loadTextPayload(request);
    if (!payload.isValid())
        return failure(payload.errorCode, payload.errorMessage);
    const QString &text = payload.text;

    const QString coordinateSpace = request.value("coordinateSpace").toString("normalized");
    if (coordinateSpace != "normalized" && coordinateSpace != "scene")
        return failure("invalid-coordinate-space", "text insertion supports normalized or scene coordinates");

    const double x = request.value("x").toDouble(0.5);
    const double y = request.value("y").toDouble(0.5);
    if (coordinateSpace == "normalized" && (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0))
        return failure("coordinate-out-of-range", "normalized x and y must be within [0,1]");

    QString operationResult;
    QString helperError;
    QString pageId;
    const bool dispatched = runOnGuiSync([&]() {
        const ActivePageContext context = findActivePage();
        if (!context.controller) {
            operationResult = "controller-not-found";
            return;
        }
        pageId = context.pageId;
        QObject *helper = ensureHelper(&helperError);
        if (!helper) {
            operationResult = "helper-unavailable";
            return;
        }
        helper->setProperty("sceneController", QVariant::fromValue(context.controller));
        QVariant result;
        if (!QMetaObject::invokeMethod(
                helper,
                "insertText",
                Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, QVariant(text)),
                Q_ARG(QVariant, QVariant(x)),
                Q_ARG(QVariant, QVariant(y)),
                Q_ARG(QVariant, QVariant(coordinateSpace)))) {
            operationResult = "invoke-failed";
            return;
        }
        operationResult = result.toString();
    });

    if (!dispatched)
        return failure("gui-unavailable", "cannot dispatch text insertion to the GUI thread");
    if (operationResult != "ok") {
        QString detail = operationResult;
        if (!helperError.isEmpty())
            detail += QStringLiteral(": ") + helperError;
        return failure("text-insert-failed", detail);
    }

    QJsonObject result{
        {"ok", true},
        {"target", "current-page"},
        {"type", "text"},
        {"source", payload.path.isEmpty() ? "inline" : "path"},
        {"pageId", pageId},
        {"coordinateSpace", coordinateSpace},
        {"x", x},
        {"y", y},
        {"characters", text.size()},
        {"representation", "typed-text"},
    };
    if (!payload.path.isEmpty())
        result.insert("path", payload.path);
    return result;
}

struct InputDevice {
    int fd = -1;
    int maxX = 0;
    int maxY = 0;
    bool rotated = false;
    QString path;

    ~InputDevice()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

bool isSafeInputPath(const QString &path)
{
    if (!path.startsWith("/dev/input/event"))
        return false;
    const QString suffix = path.mid(QStringLiteral("/dev/input/event").size());
    if (suffix.isEmpty())
        return false;
    return std::all_of(suffix.cbegin(), suffix.cend(), [](QChar c) { return c.isDigit(); });
}

bool isRemarkable2()
{
    // The reMarkable 2 is the only supported 32-bit target. Prefer that stable
    // distinction because /etc/hwrevision has used several text formats.
    const QString architecture = QSysInfo::currentCpuArchitecture();
    if (architecture.startsWith("arm") && !architecture.contains("64"))
        return true;

    QFile file("/etc/hwrevision");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return QString::fromUtf8(file.readAll()).contains("reMarkable2", Qt::CaseInsensitive);
}

std::unique_ptr<InputDevice> openInputDevice(const QString &overridePath, QString *error)
{
    auto device = std::make_unique<InputDevice>();
    device->rotated = isRemarkable2();
    device->path = overridePath.isEmpty()
        ? (device->rotated ? QStringLiteral("/dev/input/event1") : QStringLiteral("/dev/input/event2"))
        : overridePath;

    if (!isSafeInputPath(device->path)) {
        if (error)
            *error = "inputDevice must match /dev/input/event<number>";
        return nullptr;
    }

    device->fd = ::open(device->path.toUtf8().constData(), O_RDWR | O_CLOEXEC);
    if (device->fd < 0) {
        if (error)
            *error = QStringLiteral("cannot open %1: %2").arg(device->path, QString::fromLocal8Bit(std::strerror(errno)));
        return nullptr;
    }

    input_absinfo xInfo{};
    input_absinfo yInfo{};
    if (::ioctl(device->fd, EVIOCGABS(ABS_X), &xInfo) < 0 ||
        ::ioctl(device->fd, EVIOCGABS(ABS_Y), &yInfo) < 0) {
        if (error)
            *error = QStringLiteral("cannot query pen axes on %1: %2")
                         .arg(device->path, QString::fromLocal8Bit(std::strerror(errno)));
        return nullptr;
    }
    device->maxX = xInfo.maximum;
    device->maxY = yInfo.maximum;
    if (device->maxX <= 0 || device->maxY <= 0) {
        if (error)
            *error = "pen device reported invalid axis limits";
        return nullptr;
    }
    return device;
}

bool writeEvents(int fd, const input_event *events, size_t count, QString *error)
{
    const char *data = reinterpret_cast<const char *>(events);
    size_t remaining = count * sizeof(input_event);
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (error)
                *error = QString::fromLocal8Bit(std::strerror(errno));
            return false;
        }
        if (written == 0) {
            if (error)
                *error = "pen device accepted zero bytes";
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

input_event event(quint16 type, quint16 code, qint32 value)
{
    input_event result{};
    result.type = type;
    result.code = code;
    result.value = value;
    return result;
}

QPoint screenToInput(const InputDevice &device, const QPointF &point, const QSize &canvas)
{
    const double nx = std::clamp(point.x() / std::max(1, canvas.width()), 0.0, 1.0);
    const double ny = std::clamp(point.y() / std::max(1, canvas.height()), 0.0, 1.0);
    if (device.rotated) {
        return {
            qRound((1.0 - ny) * device.maxX),
            qRound(nx * device.maxY),
        };
    }
    return {
        qRound(nx * device.maxX),
        qRound(ny * device.maxY),
    };
}

bool penUp(InputDevice &device, QString *error)
{
    const input_event events[] = {
        event(EV_ABS, ABS_PRESSURE, 0),
        event(EV_ABS, ABS_DISTANCE, 100),
        event(EV_KEY, BTN_TOUCH, 0),
        event(EV_KEY, BTN_TOOL_PEN, 0),
        event(EV_SYN, SYN_REPORT, 0),
    };
    return writeEvents(device.fd, events, std::size(events), error);
}

bool penDownAt(InputDevice &device, const QPoint &point, QString *error)
{
    const input_event hover[] = {
        event(EV_ABS, ABS_X, point.x()),
        event(EV_ABS, ABS_Y, point.y()),
        event(EV_KEY, BTN_TOOL_PEN, 1),
        event(EV_KEY, BTN_TOUCH, 0),
        event(EV_ABS, ABS_PRESSURE, 0),
        event(EV_ABS, ABS_DISTANCE, 100),
        event(EV_SYN, SYN_REPORT, 0),
    };
    if (!writeEvents(device.fd, hover, std::size(hover), error))
        return false;
    const input_event down[] = {
        event(EV_KEY, BTN_TOUCH, 1),
        event(EV_ABS, ABS_PRESSURE, 2630),
        event(EV_ABS, ABS_DISTANCE, 0),
        event(EV_SYN, SYN_REPORT, 0),
    };
    return writeEvents(device.fd, down, std::size(down), error);
}

bool movePen(InputDevice &device, const QPoint &point, QString *error)
{
    const input_event events[] = {
        event(EV_ABS, ABS_X, point.x()),
        event(EV_ABS, ABS_Y, point.y()),
        event(EV_SYN, SYN_REPORT, 0),
    };
    return writeEvents(device.fd, events, std::size(events), error);
}

bool drawRun(InputDevice &device, const QPointF &from, const QPointF &to, const QSize &canvas, QString *error)
{
    const QPoint inputFrom = screenToInput(device, from, canvas);
    const QPoint inputTo = screenToInput(device, to, canvas);
    if (!penUp(device, error) || !penDownAt(device, inputFrom, error))
        return false;
    QThread::usleep(1000);

    const double distance = std::hypot(inputTo.x() - inputFrom.x(), inputTo.y() - inputFrom.y());
    const int steps = std::max(1, qCeil(distance / 80.0));
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const QPoint point(
            qRound(inputFrom.x() + (inputTo.x() - inputFrom.x()) * t),
            qRound(inputFrom.y() + (inputTo.y() - inputFrom.y()) * t));
        if (!movePen(device, point, error)) {
            penUp(device, nullptr);
            return false;
        }
    }
    QThread::usleep(1000);
    return penUp(device, error);
}

QSize activeCanvasSize()
{
    QWindow *largestVisibleWindow = nullptr;
    for (QWindow *window : QGuiApplication::allWindows()) {
        if (!window || !window->isVisible() || window->width() <= 0 || window->height() <= 0)
            continue;
        if (!largestVisibleWindow ||
            window->width() * window->height() >
                largestVisibleWindow->width() * largestVisibleWindow->height()) {
            largestVisibleWindow = window;
        }
    }
    if (largestVisibleWindow)
        return largestVisibleWindow->size();
    if (QScreen *screen = QGuiApplication::primaryScreen())
        return screen->size();
    return {768, 1024};
}

QJsonObject drawImage(const QJsonObject &request)
{
    const QString path = request.value("path").toString();
    if (path.isEmpty())
        return failure("missing-path", "image requests require a device-local path");
    QImage source = loadTransferImage(path);
    if (source.isNull())
        return failure("image-load-failed", QStringLiteral("cannot decode image: ") + path);

    QSize canvas;
    bool controllerFound = false;
    QString pageId;
    if (!runOnGuiSync([&]() {
            canvas = activeCanvasSize();
            const ActivePageContext context = findActivePage();
            controllerFound = context.controller != nullptr;
            pageId = context.pageId;
        })) {
        return failure("gui-unavailable", "cannot inspect the current page on the GUI thread");
    }
    if (!controllerFound)
        return failure("no-current-page", "no active notebook SceneController was found");

    if (request.contains("canvasWidth"))
        canvas.setWidth(request.value("canvasWidth").toInt(canvas.width()));
    if (request.contains("canvasHeight"))
        canvas.setHeight(request.value("canvasHeight").toInt(canvas.height()));
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return failure("invalid-canvas", "canvasWidth and canvasHeight must be positive");

    const QString coordinateSpace = request.value("coordinateSpace").toString("normalized");
    const bool center = request.value("center").toBool(false);
    double x = request.value("x").toDouble(0.1);
    double y = request.value("y").toDouble(0.1);
    double width = request.value("width").toDouble(coordinateSpace == "normalized" ? 0.8 : source.width());
    double height = request.value("height").toDouble(0.0);
    if (height <= 0.0)
        height = width * static_cast<double>(source.height()) / std::max(1, source.width());
    if (coordinateSpace == "normalized") {
        x *= canvas.width();
        y *= canvas.height();
        width *= canvas.width();
        if (request.contains("height"))
            height *= canvas.height();
        else
            height = width * static_cast<double>(source.height()) / std::max(1, source.width());

        if (center) {
            const double maxHeight = canvas.height() * 0.8;
            if (height > maxHeight) {
                const double scale = maxHeight / height;
                width *= scale;
                height *= scale;
            }
            x = (canvas.width() - width) / 2.0;
            y = (canvas.height() - height) / 2.0;
        }
    } else if (coordinateSpace != "screen") {
        return failure("invalid-coordinate-space", "image insertion supports normalized or screen coordinates");
    }

    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > canvas.width() || y + height > canvas.height())
        return failure("rectangle-out-of-range", "the image rectangle must fit inside the current canvas");

    const int sampleLimit = std::clamp(request.value("sampleLimit").toInt(384), 32, 768);
    QSize sampleSize(qMax(1, qRound(width)), qMax(1, qRound(height)));
    sampleSize.scale(sampleLimit, sampleLimit, Qt::KeepAspectRatio);
    QImage image = source.convertToFormat(QImage::Format_ARGB32)
                       .scaled(sampleSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const int threshold = std::clamp(request.value("threshold").toInt(150), 0, 255);
    const int alphaThreshold = std::clamp(request.value("alphaThreshold").toInt(32), 0, 255);
    const int rowStep = std::clamp(request.value("rowStep").toInt(2), 1, 16);
    const bool invert = request.value("invert").toBool(false);
    const int maxRuns = std::clamp(request.value("maxRuns").toInt(20000), 100, 50000);

    int runCount = 0;
    for (int row = 0; row < image.height(); row += rowStep) {
        bool inRun = false;
        for (int column = 0; column <= image.width(); ++column) {
            bool dark = false;
            if (column < image.width()) {
                const QRgb pixel = image.pixel(column, row);
                const int luminance = qGray(pixel);
                dark = qAlpha(pixel) >= alphaThreshold && (invert ? luminance >= threshold : luminance <= threshold);
            }
            if (dark && !inRun) {
                inRun = true;
            } else if (!dark && inRun) {
                inRun = false;
                ++runCount;
                if (runCount > maxRuns)
                    return failure("image-too-complex", "thresholded image exceeds maxRuns; reduce sampleLimit or increase rowStep");
            }
        }
    }
    if (runCount == 0)
        return failure("image-empty", "no drawable pixels remain after thresholding");

    if (!g_inputMutex.tryLock())
        return failure("busy", "another input injection is already running");
    const std::unique_ptr<QMutex, void (*)(QMutex *)> locker(
        &g_inputMutex,
        [](QMutex *mutex) { mutex->unlock(); });

    QString inputError;
    std::unique_ptr<InputDevice> device = openInputDevice(request.value("inputDevice").toString(), &inputError);
    if (!device)
        return failure("pen-device-unavailable", inputError);

    QElapsedTimer timer;
    timer.start();
    int drawnRuns = 0;
    for (int row = 0; row < image.height(); row += rowStep) {
        int runStart = -1;
        for (int column = 0; column <= image.width(); ++column) {
            bool dark = false;
            if (column < image.width()) {
                const QRgb pixel = image.pixel(column, row);
                const int luminance = qGray(pixel);
                dark = qAlpha(pixel) >= alphaThreshold && (invert ? luminance >= threshold : luminance <= threshold);
            }
            if (dark && runStart < 0) {
                runStart = column;
            } else if (!dark && runStart >= 0) {
                const int runEnd = column - 1;
                const double screenY = y + (static_cast<double>(row) + 0.5) * height / image.height();
                double screenX1 = x + static_cast<double>(runStart) * width / image.width();
                double screenX2 = x + static_cast<double>(runEnd + 1) * width / image.width();
                if (screenX2 - screenX1 < 1.0)
                    screenX2 = std::min(x + width, screenX1 + 1.0);
                if (!drawRun(*device, {screenX1, screenY}, {screenX2, screenY}, canvas, &inputError)) {
                    penUp(*device, nullptr);
                    return failure("pen-write-failed", inputError);
                }
                ++drawnRuns;
                runStart = -1;
            }
        }
    }

    return {
        {"ok", true},
        {"target", "current-page"},
        {"type", "image"},
        {"pageId", pageId},
        {"path", path},
        {"representation", "editable-ink"},
        {"coordinateSpace", "screen"},
        {"canvasWidth", canvas.width()},
        {"canvasHeight", canvas.height()},
        {"x", x},
        {"y", y},
        {"width", width},
        {"height", height},
        {"runs", drawnRuns},
        {"elapsedMs", timer.elapsed()},
        {"warning", "drawing uses the currently selected xochitl writing tool"},
    };
}

QJsonObject capabilities()
{
    QSize canvas;
    bool controllerFound = false;
    runOnGuiSync([&]() {
        canvas = activeCanvasSize();
        controllerFound = findActivePage().controller != nullptr;
    });

    return {
        {"ok", true},
        {"apiVersion", 1},
        {"maxBrokerRequestBytes", 1024},
        {"currentPageAvailable", controllerFound},
        {"canvas", QJsonObject{{"width", canvas.width()}, {"height", canvas.height()}}},
        {"currentPage", QJsonObject{
            {"text", "typed-text"},
            {"image", QJsonArray{"scene-image", "editable-ink"}},
            {"imageFormats", QJsonArray{"png", "jpeg", "webp", "svg"}},
            {"defaultImagePlacement", "center"},
            {"textSources", QJsonArray{"inline", "path"}},
            {"textCoordinateSpaces", QJsonArray{"normalized", "scene"}},
            {"sceneImageCoordinateSpaces", QJsonArray{"normalized", "scene"}},
            {"inkImageCoordinateSpaces", QJsonArray{"normalized", "screen"}},
        }},
    };
}

QJsonObject handlePut(const char *value)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(value ? value : ""), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failure("invalid-json", parseError.errorString());

    const QJsonObject request = document.object();
    const QString target = request.value("target").toString();
    const QString type = request.value("type").toString();
    if (target != "current-page")
        return failure("invalid-target", "target must be current-page");
    if (type != "text" && type != "image")
        return failure("invalid-type", "type must be text or image");

    if (target == "current-page" && type == "text")
        return insertText(request);
    const QString representation = request.value("representation").toString("scene-image");
    if (representation == "scene-image")
        return insertSceneImage(request);
    if (representation == "editable-ink")
        return drawImage(request);
    return failure("invalid-representation", "image representation must be scene-image or editable-ink");
}

} // namespace

extern "C" char *xovi_content_inserter_put(const char *value)
{
    qInfo("[xovi-content-inserter] put request received");
    return response(handlePut(value));
}

extern "C" char *xovi_content_inserter_capabilities(const char *)
{
    qInfo("[xovi-content-inserter] capabilities request received");
    return response(capabilities());
}

extern "C" void _xovi_construct()
{
    qInfo("[xovi-content-inserter] loaded");
    if (Environment) {
        const int extensionCount = Environment->getExtensionCount ? Environment->getExtensionCount() : -1;
        const int functionCount = Environment->getExtensionFunctionCount
            ? Environment->getExtensionFunctionCount("xovi-content-inserter")
            : -1;
        XoviMetadataEntry *putSignal = Environment->getMetadataEntryForFunction
            ? Environment->getMetadataEntryForFunction(
                "xovi-content-inserter", "xovi_content_inserter_put",
                LP1_F_TYPE_EXPORT, "xovi-message-broker$simpleSignal")
            : nullptr;
        XoviMetadataEntry *capabilitiesSignal = Environment->getMetadataEntryForFunction
            ? Environment->getMetadataEntryForFunction(
                "xovi-content-inserter", "xovi_content_inserter_capabilities",
                LP1_F_TYPE_EXPORT, "xovi-message-broker$simpleSignal")
            : nullptr;
        qInfo("[xovi-content-inserter] XOVI metadata: extensions=%d functions=%d put=%p capabilities=%p",
            extensionCount, functionCount, putSignal, capabilitiesSignal);
    }
    if (Environment && Environment->createMetadataSearchingIterator && Environment->nextFunctionMetadataEntry) {
        ExtensionMetadataIterator iterator{};
        Environment->createMetadataSearchingIterator(&iterator, "xovi-message-broker$simpleSignal");
        while (XoviMetadataEntry *entry = Environment->nextFunctionMetadataEntry(&iterator)) {
            const QString signal = QString::fromUtf8(entry->value.s, entry->value.sLength);
            if (signal.startsWith("xovi-content-inserter")) {
                qInfo("[xovi-content-inserter] registered broker signal %s as %s/%s at %p",
                    qPrintable(signal), iterator.extensionName, iterator.functionName, iterator.functionAddress);
            }
        }
    }
}

extern "C" char _xovi_shouldLoad()
{
    return dlsym(RTLD_DEFAULT, "_Z21qRegisterResourceDataiPKhS0_S0_") != nullptr;
}
