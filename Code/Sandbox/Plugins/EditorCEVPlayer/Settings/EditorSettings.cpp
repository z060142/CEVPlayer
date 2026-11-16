#include "StdAfx.h"
#include "Settings/EditorSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

namespace CEVEditor
{
	namespace
	{
		QJsonObject ReadObject(const QString& path)
		{
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
				return QJsonObject();
			const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
			return doc.isObject() ? doc.object() : QJsonObject();
		}
	}

	std::string EditorSettingsPath()
	{
		const QByteArray overridePath = qgetenv("CEV_EDITOR_SETTINGS");
		if (!overridePath.isEmpty())
			return std::string(overridePath.constData());
		QString base = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
		if (base.isEmpty())
			base = QDir::homePath() + QStringLiteral("/AppData/Local");
		return std::string((QDir::fromNativeSeparators(base) + QStringLiteral("/CEVPlayer/editor.json")).toUtf8().constData());
	}

	std::string LoadEditorSetting(const char* key)
	{
		const QJsonObject obj = ReadObject(QString::fromUtf8(EditorSettingsPath().c_str()));
		return std::string(obj.value(QString::fromUtf8(key)).toString().toUtf8().constData());
	}

	bool SaveEditorSetting(const char* key, const std::string& value, std::string& error)
	{
		const QString path = QString::fromUtf8(EditorSettingsPath().c_str());
		QJsonObject obj = ReadObject(path);
		if (value.empty())
			obj.remove(QString::fromUtf8(key));
		else
			obj.insert(QString::fromUtf8(key), QString::fromUtf8(value.c_str()));
		QDir().mkpath(QFileInfo(path).absolutePath());
		QSaveFile file(path); // written to a temporary file and renamed: a crash never leaves half a file
		if (!file.open(QIODevice::WriteOnly))
		{
			error = std::string("cannot write ") + path.toUtf8().constData() + ": " + file.errorString().toUtf8().constData();
			return false;
		}
		file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
		if (!file.commit())
		{
			error = std::string("cannot write ") + path.toUtf8().constData() + ": " + file.errorString().toUtf8().constData();
			return false;
		}
		return true;
	}
}
