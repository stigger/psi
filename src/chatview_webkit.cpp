/*
 * chatview_webkit.cpp - Webkit based chatview
 * Copyright (C) 2010  Rion
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "chatview_webkit.h"

#include "msgmle.h"
#include "psioptions.h"
#include "textutil.h"

#include <QWidget>
#if QT_WEBENGINEWIDGETS_LIB
#if QT_VERSION >= QT_VERSION_CHECK(5,7,0)
#include <QWebEngineContextMenuData>
#endif
#include <QWebEngineSettings>
#else
#include <QWebFrame>
#endif
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QPalette>
#include <QDesktopWidget>
#include <QApplication>

#include "webview.h"
//#include "psiapplication.h"
#include "psiaccount.h"
#include "applicationinfo.h"
#include "networkaccessmanager.h"
#include "jsutil.h"
#include "messageview.h"
#include "psithememanager.h"
#include "chatviewtheme.h"
#include "chatviewthemeprovider.h"
#include "avatars.h"


class ChatViewThemeSessionBridge;
class ChatViewJSObject;
class ChatViewPrivate
{
public:
	ChatViewPrivate() {}

	ChatViewTheme theme;
	QSharedPointer<ChatViewThemeSessionBridge> themeBridge;

	WebView *webView;
	ChatViewJSObject *jsObject;
	QStringList jsBuffer_;
	bool sessionReady_ = false;
	QPointer<QWidget> dialog_ = 0;
	bool isMuc_ = false;
	bool isEncryptionEnabled_ = false;
	Jid jid_;
	QString name_;
	PsiAccount *account_;
	AvatarFactory::UserHashes remoteIcons;
	AvatarFactory::UserHashes localIcons;
};


//----------------------------------------------------------------------------
// ChatViewJSObject
// object which will be embed to javascript part of view
//----------------------------------------------------------------------------
class ChatViewJSObject : public QObject
{
	Q_OBJECT

	ChatView *_view;

	Q_PROPERTY(bool isMuc READ isMuc CONSTANT)
	Q_PROPERTY(QString chatName READ chatName CONSTANT)
	Q_PROPERTY(QString jid READ jid CONSTANT)
	Q_PROPERTY(QString account READ account CONSTANT)
	Q_PROPERTY(QString remoteUserImage READ remoteUserImage NOTIFY remoteUserImageChanged) // associated with chat(e.g. MUC's own avatar)
	Q_PROPERTY(QString remoteUserAvatar READ remoteUserAvatar NOTIFY remoteUserAvatarChanged) // remote avatar. resized vcard or PEP.
	Q_PROPERTY(QString localUserImage READ localUserImage NOTIFY localUserImageChanged)    // local image. from vcard
	Q_PROPERTY(QString localUserAvatar READ localUserAvatar NOTIFY localUserAvatarChanged) // local avatar. resized vcard or PEP.


public:
	ChatViewJSObject(ChatView *view) :
		QObject(view),
		_view(view)
	{

	}

	bool isMuc() const
	{
		return _view->d->isMuc_;
	}

	QString chatName() const
	{
		return _view->d->name_;
	}

	QString jid() const
	{
		return _view->d->jid_.full();
	}

	QString account() const
	{
		return _view->d->account_->id();
	}

	inline static QString avatarUrl(const QString &hash)
	{ return hash.isEmpty()? QString() : QLatin1String("/psiglobal/avatar/") + hash; }

	QString remoteUserImage()  const { return avatarUrl(_view->d->remoteIcons.vcard);  }
	QString remoteUserAvatar() const { return avatarUrl(_view->d->remoteIcons.avatar); }
	QString localUserImage()   const { return avatarUrl(_view->d->localIcons.vcard);   }
	QString localUserAvatar()  const { return avatarUrl(_view->d->localIcons.avatar);  }

	void setRemoteUserAvatarHash(const QString &hash)
	{ emit remoteUserAvatarChanged(hash.isEmpty()? hash : avatarUrl(hash)); }
	void setRemoteUserImageHash(const QString &hash)
	{ emit remoteUserImageChanged(hash.isEmpty()? hash : avatarUrl(hash)); }
	void setLocalUserAvatarHash(const QString &hash)
	{ emit localUserAvatarChanged(hash.isEmpty()? hash : avatarUrl(hash)); }
	void setLocalUserImageHash(const QString &hash)
	{ emit localUserImageChanged(hash.isEmpty()? hash : avatarUrl(hash)); }

public slots:
	QString mucNickColor(QString nick, bool isSelf,
						 QStringList validList = QStringList()) const
	{
		return _view->getMucNickColor(nick, isSelf, validList);
	}

	void signalInited()
	{
		emit inited();
	}

	QString getFont() const
	{
		QFont f = ((ChatView*)parent())->font();
		QString weight = "normal";
		switch (f.weight()) {
			case QFont::Light: weight = "lighter"; break;
			case QFont::DemiBold: weight = "bold"; break;
			case QFont::Bold: weight = "bolder"; break;
			case QFont::Black: weight = "900"; break;
		}

		// In typography 1 point (also called PostScript point)
		// is 1/72 of an inch
		const float postScriptPoint = 1 / 72.;

		// Workaround.  WebKit works only with 96dpi
		// Need to convert point size to pixel size
		int pixelSize = qRound(f.pointSize() * qApp->desktop()->logicalDpiX() * postScriptPoint);

		return QString("{fontFamily:'%1',fontSize:'%2px',fontStyle:'%3',fontVariant:'%4',fontWeight:'%5'}")
						 .arg(f.family())
						 .arg(pixelSize)
						 .arg(f.style()==QFont::StyleNormal?"normal":(f.style()==QFont::StyleItalic?"italic":"oblique"))
						 .arg(f.capitalization() == QFont::SmallCaps?"small-caps":"normal")
						 .arg(weight);
	}

	QString getPaletteColor(const QString &name) const
	{
		QPalette::ColorRole cr = QPalette::NoRole;

		if (name == "WindowText") {
			cr = QPalette::WindowText;
		} else if (name == "Text") {
			cr = QPalette::Text;
		} else if (name == "Base") {
			cr = QPalette::Base;
		} else if (name == "Window") {
			cr = QPalette::Window;
		} else if (name == "Highlight") {
			cr = QPalette::Highlight;
		} else if (name == "HighlightedText") {
			cr = QPalette::HighlightedText;
		} else if (name.endsWith("Text")) {
			cr = QPalette::Text;
		} else {
			cr = QPalette::Base;
		}

		return _view->palette().color(cr).name();
	}

signals:
	void inited(); // signal from this object to C++. Means ready to process messages
	void scrollRequested(int); // relative shift. signal towards js
	void remoteUserImageChanged(const QString &);
	void remoteUserAvatarChanged(const QString &);
	void localUserImageChanged(const QString &);
	void localUserAvatarChanged(const QString &);
};

//----------------------------------------------------------------------------
// ChatView
//----------------------------------------------------------------------------
class ChatViewThemeSessionBridge : public ChatViewThemeSession
{
	ChatView *cv;
public:
	ChatViewThemeSessionBridge(ChatView *cv) : cv(cv) {}

	// returns: data, content-type
	QPair<QByteArray,QByteArray> getContents(const QUrl &url)
	{
		QString path = url.path();
		if (path.startsWith(QLatin1String("/psiglobal/avatar/"))) {
			QString hash = path.mid(sizeof("/psiglobal/avatar")); // no / because of null pointer
			QString meta;
			QByteArray ba;
			if (hash == QLatin1String("default.png")) {
				QPixmap p;
				QBuffer buffer(&ba);
				buffer.open(QIODevice::WriteOnly);
				p = IconsetFactory::icon(QLatin1String("psi/default_avatar")).pixmap();
				p.save(&buffer, "PNG");
				meta = QLatin1String("image/png");
			} else {
				AvatarFactory::AvatarData ad = cv->d->account_->avatarFactory()->avatarDataByHash(hash);
				ba = ad.data;
				meta = ad.metaType;
			}
			if (!ba.isEmpty()) {
				return QPair<QByteArray,QByteArray>(ba, meta.toLatin1());
			}
		}
		return QPair<QByteArray,QByteArray>();
	}

	WebView* webView()
	{
		return cv->textWidget();
	}

	QObject* jsBridge()
	{
		return cv->jsBridge();
	}

};




//----------------------------------------------------------------------------
// ChatView
//----------------------------------------------------------------------------
ChatView::ChatView(QWidget *parent) :
    QFrame(parent),
    d(new ChatViewPrivate)
{
	d->themeBridge.reset(new ChatViewThemeSessionBridge(this));


	d->jsObject = new ChatViewJSObject(this); /* It's a session bridge between html and c++ part */
	d->webView = new WebView(this);
	d->webView->setFocusPolicy(Qt::NoFocus);
#ifndef QT_WEBENGINEWIDGETS_LIB
	d->webView->settings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, true);
#endif
	QVBoxLayout *layout = new QVBoxLayout;
	layout->setContentsMargins(0,0,0,0);
	layout->addWidget(d->webView);
	setLayout(layout);
	setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
	setLooks(d->webView);

#ifndef HAVE_X11	// linux has this feature built-in
	connect( PsiOptions::instance(), SIGNAL(optionChanged(QString)), SLOT(psiOptionChanged(QString)) ); //needed only for save autocopy state atm
	psiOptionChanged("options.ui.automatically-copy-selected-text"); // init autocopy connection
#endif
	connect(d->jsObject, SIGNAL(inited()), SLOT(sessionInited()));
#if QT_WEBENGINEWIDGETS_LIB
	// TODO something
#else

#endif
}

ChatView::~ChatView()
{
}

// something after we know isMuc and dialog is set. kind of final step
void ChatView::init()
{
	d->themeBridge.reset(new ChatViewThemeSessionBridge(this));
	d->theme.applyToWebView(d->themeBridge.dynamicCast<ChatViewThemeSession>());
	if (d->theme.isTransparentBackground()) {
		QWidget *w = this;
		while (w) {
			w->setAttribute(Qt::WA_TranslucentBackground, true);
			w = w->parentWidget();
		}
	}
}

void ChatView::setEncryptionEnabled(bool enabled)
{
	d->isEncryptionEnabled_ = enabled;
}

#ifndef QT_WEBENGINEWIDGETS_LIB
void ChatView::embedJsObject()
{
	ChatViewTheme *theme = currentTheme();
	QWebFrame *wf = webView->page()->mainFrame();
	wf->addToJavaScriptWindowObject("chatServer", theme->jsHelper());
	wf->addToJavaScriptWindowObject("chatSession", jsObject);
	foreach (const QString &script, theme->scripts()) {
		wf->evaluateJavaScript(script);
	}
}
#endif

void ChatView::markReceived(QString id)
{
	QVariantMap m;
	m["type"] = "receipt";
	m["id"] = id;
	m["encrypted"] = d->isEncryptionEnabled_;
	sendJsObject(m);
}

QSize ChatView::sizeHint() const
{
	return minimumSizeHint();
}

void ChatView::setDialog(QWidget* dialog)
{
	d->dialog_ = dialog;
}

void ChatView::setSessionData(bool isMuc, const Jid &jid, const QString name)
{
	auto provider = (ChatViewThemeProvider *)PsiThemeManager::instance()->
	                provider(d->isMuc_?"groupchatview":"chatview");
	d->isMuc_ = isMuc;
	d->jid_ = jid;
	d->name_ = name;
	d->theme = *(dynamic_cast<ChatViewTheme*>(provider->current()));
	connect(provider, SIGNAL(themeChanged()), SLOT(init()));
}

void ChatView::setAccount(PsiAccount *acc)
{
	d->account_ = acc;
	d->remoteIcons = acc->avatarFactory()->userHashes(d->jid_);
	d->localIcons = acc->avatarFactory()->userHashes(acc->jid());
}

void ChatView::contextMenuEvent(QContextMenuEvent *e)
{
#if defined(QT_WEBENGINEWIDGETS_LIB) && QT_VERSION < QT_VERSION_CHECK(5,7,0)
	Q_UNUSED(e)
	qDebug("Can't check menu hit point. Calling default handler");
#else
	QUrl linkUrl;
# ifdef QT_WEBENGINEWIDGETS_LIB
	QWebEngineContextMenuData cmd = d->webView->page()->contextMenuData();
	linkUrl = cmd.linkUrl();
# else
	linkUrl = d->webView->page()->mainFrame()->hitTestContent(e->pos()).linkUrl();
# endif
	if ( linkUrl.scheme() == "addnick" ) {
		showNM(linkUrl.path().mid(1));
		e->accept();
	}
#endif
}

bool ChatView::focusNextPrevChild(bool next)
{
	return QWidget::focusNextPrevChild(next);
}

void ChatView::changeEvent(QEvent * event)
{
	if ( event->type() == QEvent::ApplicationPaletteChange
		|| event->type() == QEvent::PaletteChange
		|| event->type() == QEvent::FontChange ) {
		QVariantMap m;
		m["type"] = "settings";
		sendJsObject(m);
	}
	QFrame::changeEvent(event);
}

void ChatView::psiOptionChanged(const QString &option)
{
	if (option == "options.ui.automatically-copy-selected-text") {
		if (PsiOptions::instance()->
			getOption("options.ui.automatically-copy-selected-text").toBool()) {
			connect(d->webView->page(), SIGNAL(selectionChanged()), d->webView, SLOT(copySelected()));
		} else {
			disconnect(d->webView->page(), SIGNAL(selectionChanged()), d->webView, SLOT(copySelected()));
		}
	}
}

void ChatView::sendJsObject(const QVariantMap &map)
{
	sendJsCommand(QString(d->theme.jsNamespace() + ".adapter.receiveObject(%1);")
				  .arg(JSUtil::map2json(map)));
}

void ChatView::sendJsCommand(const QString &cmd)
{
	d->jsBuffer_.append(cmd);
	checkJsBuffer();
}

void ChatView::checkJsBuffer()
{
	if (d->sessionReady_) {
		while (!d->jsBuffer_.isEmpty()) {
			d->webView->evaluateJS(d->jsBuffer_.takeFirst());
		}
	}
}

void ChatView::sessionInited()
{
	d->sessionReady_ = true;
	checkJsBuffer();
}

bool ChatView::handleCopyEvent(QObject *object, QEvent *event, ChatEdit *chatEdit) {
	if (object == chatEdit && event->type() == QEvent::ShortcutOverride &&
		((QKeyEvent*)event)->matches(QKeySequence::Copy)) {

		if (!chatEdit->textCursor().hasSelection() &&
			 !(d->webView->page()->selectedText().isEmpty()))
		{
			d->webView->copySelected();
			return true;
		}
	}

	return false;
}

// input point of all messages
void ChatView::dispatchMessage(const MessageView &mv)
{
	QString replaceId = mv.replaceId();
	if (replaceId.isEmpty()) {
		if ((mv.type() == MessageView::Message || mv.type() == MessageView::Subject)
				&& updateLastMsgTime(mv.dateTime())) {
			QVariantMap m;
			m["date"] = mv.dateTime();
			m["type"] = "message";
			m["mtype"] = "lastDate";
			sendJsObject(m);
		}
		QVariantMap vm = mv.toVariantMap(d->isMuc_, true);
		vm["mtype"] = vm["type"];
		vm["type"] = "message";
		vm["encrypted"] = d->isEncryptionEnabled_;
		sendJsObject(vm);
	} else {
		QString msgId = TextUtil::escape("msgid_" + replaceId + "_" + mv.userId());
		QString replaceText = mv.formattedText().replace("\"", "\\\"");
		// TODO: move to JS adapters and add smilies/icons support
		QString jsCommand =
				QString(
						"var msgs = document.querySelectorAll(\"a[name=\\\"%1\\\"]\");"
						"if (msgs) {"
						"  var elem = msgs[msgs.length - 1].previousSibling;"
						"  while (next = elem.nextSibling) {"
						"    next.remove();"
						"  }"
						"  var oldText = elem.innerHTML.replace(/<[^>]*>/gi, \"\");"
						"  elem.outerHTML = \"%2\" + \"<img src='icon:psi/action_templates_edit' title='\" + oldText + \"' />\";"
						"} else {"
						"  console.log(\"Messages with name %1 not found\");"
						"}").arg(
						msgId).arg(replaceText);
		sendJsCommand(jsCommand);
	}
}

void ChatView::scrollUp()
{
	emit d->jsObject->scrollRequested(-50);
}

void ChatView::scrollDown()
{
	emit d->jsObject->scrollRequested(50);
}

void ChatView::updateAvatar(const Jid &jid, ChatViewCommon::UserType utype)
{
	bool avatarChanged = false;
	bool vcardChanged = false;

	if (utype == RemoteParty) { // remote party but not muc participant
		auto h = d->account_->avatarFactory()->userHashes(jid);
		avatarChanged = h.avatar != d->remoteIcons.avatar;
		vcardChanged = h.vcard != d->remoteIcons.vcard;
		d->remoteIcons = h;
		if (avatarChanged) {
			d->jsObject->setRemoteUserAvatarHash(h.avatar);
		}
		if (vcardChanged) {
			d->jsObject->setRemoteUserAvatarHash(h.avatar);
		}
	} else if (utype == LocalParty) { // local party
		auto h = d->account_->avatarFactory()->userHashes(jid);
		avatarChanged = (h.avatar != d->localIcons.avatar);
		vcardChanged = (h.vcard != d->localIcons.vcard);
		d->localIcons = h;
		if (avatarChanged) {
			d->jsObject->setLocalUserAvatarHash(h.avatar);
		}
		if (vcardChanged) {
			d->jsObject->setLocalUserImageHash(h.avatar);
		}
	} else { // muc participant
		QVariantMap m;
		m["type"] = "avatar";
		m["sender"] = jid.resource();
		m["avatar"] = ChatViewJSObject::avatarUrl(d->account_->avatarFactory()->userHashes(jid).avatar);
		sendJsObject(m);
	}
}

void ChatView::clear()
{
	QVariantMap m;
	m["type"] = "clear";
	sendJsObject(m);
}

void ChatView::doTrackBar()
{
	QVariantMap m;
	m["type"] = "trackbar";
	sendJsObject(m);
}

bool ChatView::internalFind(QString str, bool startFromBeginning)
{
#ifdef QT_WEBENGINEWIDGETS_LIB
	d->webView->page()->findText(str, QWebEnginePage::FindFlags(), [this, startFromBeginning](bool found) {
		if (!found && startFromBeginning) {
			d->webView->page()->findText(QString());
		}
    });
	return false;
#warning "TODO: make search asynchronous in all cases"
#else
	bool found = d->webView->page()->findText(str, startFromBeginning ?
				 QWebPage::FindWrapsAroundDocument : (QWebPage::FindFlag)0);

	if (!found && !startFromBeginning) {
		return internalFind(str, true);
	}

	return found;
#endif
}

WebView* ChatView::textWidget()
{
	return d->webView;
}

QWidget* ChatView::realTextWidget()
{
	return d->webView;
}

QObject *ChatView::jsBridge()
{
	return d->jsObject;
}

#include "chatview_webkit.moc"
