#pragma once

#include "device-types.hpp"

#include <QString>
#include <QVector>

namespace netdev {

struct StreamOptionEntry {
    QString id;
    QString label;
    QString profileToken;
    QString profileName;
    QString uri;
    QString snapshotUri;
    QString suggestedTransport;
    QString suggestedInputFormat;
};

struct ResolvedStreamParams {
    QString effectiveUri;
    QString inputFormat;
    QString ffmpegOptions;
    int bufferingMb = 0;
    bool asyncUnbuffered = true;
    bool asyncDecoupled = true;
    bool useHttpSnapshot = false;
};

QString serializeMediaProfiles(const QVector<MediaProfile> &profiles);
QVector<MediaProfile> deserializeMediaProfiles(const QString &json);

QVector<StreamOptionEntry> buildSelectableStreamOptions(const QVector<MediaProfile> &profiles);
QString findMatchingStreamOptionId(const QVector<StreamOptionEntry> &entries, const SourceConfig &cfg);
ResolvedStreamParams buildResolvedStreamParams(const SourceConfig &cfg);

} // namespace netdev
