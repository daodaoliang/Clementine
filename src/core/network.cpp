/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "network.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>

#include "utilities.h"

QMutex ThreadSafeNetworkDiskCache::sMutex;
QNetworkDiskCache* ThreadSafeNetworkDiskCache::sCache = NULL;


ThreadSafeNetworkDiskCache::ThreadSafeNetworkDiskCache(QObject* parent) {
  QMutexLocker l(&sMutex);
  if (!sCache) {
    sCache = new QNetworkDiskCache;
    sCache->setCacheDirectory(Utilities::GetConfigPath(Utilities::Path_NetworkCache));
  }
}

qint64 ThreadSafeNetworkDiskCache::cacheSize() const {
  QMutexLocker l(&sMutex);
  return sCache->cacheSize();
}

QIODevice* ThreadSafeNetworkDiskCache::data(const QUrl& url) {
  QMutexLocker l(&sMutex);
  return sCache->data(url);
}

void ThreadSafeNetworkDiskCache::insert(QIODevice* device) {
  QMutexLocker l(&sMutex);
  sCache->insert(device);
}

QNetworkCacheMetaData ThreadSafeNetworkDiskCache::metaData(const QUrl& url) {
  QMutexLocker l(&sMutex);
  return sCache->metaData(url);
}

QIODevice* ThreadSafeNetworkDiskCache::prepare(const QNetworkCacheMetaData& metaData) {
  QMutexLocker l(&sMutex);
  return sCache->prepare(metaData);
}

bool ThreadSafeNetworkDiskCache::remove(const QUrl& url) {
  QMutexLocker l(&sMutex);
  return sCache->remove(url);
}

void ThreadSafeNetworkDiskCache::updateMetaData(const QNetworkCacheMetaData& metaData) {
  QMutexLocker l(&sMutex);
  sCache->updateMetaData(metaData);
}

void ThreadSafeNetworkDiskCache::clear() {
  QMutexLocker l(&sMutex);
  sCache->clear();
}


NetworkAccessManager::NetworkAccessManager(QObject* parent)
  : QNetworkAccessManager(parent)
{
  setCache(new ThreadSafeNetworkDiskCache(this));
}

QNetworkReply* NetworkAccessManager::createRequest(
    Operation op, const QNetworkRequest& request, QIODevice* outgoingData) {
  QNetworkRequest new_request(request);
  new_request.setRawHeader("User-Agent", QString("%1 %2").arg(
      QCoreApplication::applicationName(),
      QCoreApplication::applicationVersion()).toUtf8());

  if (op == QNetworkAccessManager::PostOperation &&
      !new_request.header(QNetworkRequest::ContentTypeHeader).isValid()) {
    new_request.setHeader(QNetworkRequest::ContentTypeHeader,
                          "application/x-www-form-urlencoded");
  }

  // Prefer the cache unless the caller has changed the setting already
  if (request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt()
      == QNetworkRequest::PreferNetwork) {
    new_request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
  }

  return QNetworkAccessManager::createRequest(op, new_request, outgoingData);
}


NetworkTimeouts::NetworkTimeouts(int timeout_msec, QObject* parent)
  : timeout_msec_(timeout_msec) {
}

void NetworkTimeouts::AddReply(QNetworkReply* reply) {
  if (timers_.contains(reply))
    return;

  connect(reply, SIGNAL(destroyed()), SLOT(ReplyFinished()));
  connect(reply, SIGNAL(finished()), SLOT(ReplyFinished()));
  timers_[reply] = startTimer(timeout_msec_);
}

void NetworkTimeouts::ReplyFinished() {
  QNetworkReply* reply = reinterpret_cast<QNetworkReply*>(sender());
  if (timers_.contains(reply)) {
    killTimer(timers_.take(reply));
  }
}

void NetworkTimeouts::timerEvent(QTimerEvent* e) {
  QNetworkReply* reply = timers_.key(e->timerId());
  if (reply) {
    reply->abort();
  }
}


RedirectFollower::RedirectFollower(QNetworkReply* first_reply, int max_redirects)
  : QObject(NULL),
    current_reply_(first_reply),
    redirects_remaining_(max_redirects) {
  ConnectReply(first_reply);
}

void RedirectFollower::ConnectReply(QNetworkReply* reply) {
  connect(reply, SIGNAL(readyRead()), SIGNAL(readyRead()));
  connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), SIGNAL(error(QNetworkReply::NetworkError)));
  connect(reply, SIGNAL(downloadProgress(qint64,qint64)), SIGNAL(downloadProgress(qint64,qint64)));
  connect(reply, SIGNAL(uploadProgress(qint64,qint64)), SIGNAL(uploadProgress(qint64,qint64)));
  connect(reply, SIGNAL(finished()), SLOT(ReplyFinished()));
}

void RedirectFollower::ReplyFinished() {
  current_reply_->deleteLater();

  if (current_reply_->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
    if (redirects_remaining_-- == 0) {
      emit finished();
      return;
    }

    const QUrl next_url = current_reply_->url().resolved(
          current_reply_->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());

    QNetworkRequest req(current_reply_->request());
    req.setUrl(next_url);

    current_reply_ = current_reply_->manager()->get(req);
    ConnectReply(current_reply_);
    return;
  }

  emit finished();
}
