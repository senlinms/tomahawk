/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010 Leo Franchi <lfranchi@kde.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LOADING_SPINNER_H
#define LOADING_SPINNER_H

#include <QWidget>

#include "dllmacro.h"

class QMovie;
class QTimeLine;
/**
 * A small widget that displays an animated loading spinner
 */
class DLLEXPORT LoadingSpinner : public QWidget {
    Q_OBJECT
public:
    LoadingSpinner( QWidget* parent );
    virtual ~LoadingSpinner();

    virtual QSize sizeHint() const;
    virtual void paintEvent( QPaintEvent* );

public slots:
    void fadeIn();
    void fadeOut();

private slots:
    void hideFinished();

private:
    void reposition();

    QTimeLine* m_showHide;
    QMovie* m_anim;
};

#endif

class QPaintEvent;
