/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "cmakeopenprojectwizard.h"
#include "cmakeprojectmanager.h"
#include "cmakeprojectconstants.h"
#include "cmakeproject.h"

#include <utils/synchronousprocess.h>
#include <utils/qtcprocess.h>

#include <coreplugin/icore.h>
#include <coreplugin/id.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/coreconstants.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/project.h>
#include <projectexplorer/target.h>
#include <projectexplorer/session.h>
#include <utils/QtConcurrentTools>
#include <QtConcurrentRun>
#include <QCoreApplication>
#include <QSettings>
#include <QDateTime>
#include <QFormLayout>
#include <QBoxLayout>
#include <QDesktopServices>
#include <QApplication>
#include <QLabel>
#include <QGroupBox>
#include <QSpacerItem>
#include <QSignalMapper>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QMessageBox>

using namespace CMakeProjectManager::Internal;

CMakeManager::CMakeManager(CMakeSettingsPage *cmakeSettingsPage)
    : m_settingsPage(cmakeSettingsPage)
{
    ProjectExplorer::ProjectExplorerPlugin *projectExplorer = ProjectExplorer::ProjectExplorerPlugin::instance();
    connect(projectExplorer, SIGNAL(aboutToShowContextMenu(ProjectExplorer::Project*,ProjectExplorer::Node*)),
            this, SLOT(updateContextMenu(ProjectExplorer::Project*,ProjectExplorer::Node*)));

    Core::ActionContainer *mbuild =
            Core::ActionManager::actionContainer(ProjectExplorer::Constants::M_BUILDPROJECT);
    Core::ActionContainer *mproject =
            Core::ActionManager::actionContainer(ProjectExplorer::Constants::M_PROJECTCONTEXT);
    Core::ActionContainer *msubproject =
            Core::ActionManager::actionContainer(ProjectExplorer::Constants::M_SUBPROJECTCONTEXT);

    const Core::Context projectContext(CMakeProjectManager::Constants::PROJECTCONTEXT);

    m_runCMakeAction = new QAction(QIcon(), tr("Run CMake"), this);
    Core::Command *command = Core::ActionManager::registerAction(m_runCMakeAction,
                                                                 Constants::RUNCMAKE, projectContext);

    command->setAttribute(Core::Command::CA_Hide);
    connect(m_runCMakeAction, SIGNAL(triggered()), this, SLOT(runCMake()));

	Core::ActionContainer *menu = Core::ActionManager::createMenu(Constants::PROJECTCONTEXT);
	menu->menu()->setTitle(tr("CMake"));
	menu->addAction(command);
	menu->addSeparator(projectContext);
	mproject->addMenu(menu);
	msubproject->addMenu(menu);
	mbuild->addMenu(menu);

	// Build CMake context menu
	QAction *debugAct = new QAction(tr("Debug"), this);
	QAction *releaseAct = new QAction(tr("Release"), this);
	QAction *minsizerelAct = new QAction(tr("Minsizerel"), this);
	QAction *relwithdebinfoAct = new QAction(tr("Relwithdebinfo"), this);

	Core::Command *cmdDebug = Core::ActionManager::registerAction(debugAct, Constants::DEBUG_ACTION_ID,projectContext);
	Core::Command *cmdRel = Core::ActionManager::registerAction(releaseAct, Constants::RELEASE_ACTION_ID,projectContext);
	Core::Command *cmdMin = Core::ActionManager::registerAction(minsizerelAct, Constants::MINSIZE_ACTION_ID,projectContext);
	Core::Command *cmdReldeb = Core::ActionManager::registerAction(relwithdebinfoAct, Constants::RELWITHDEB_ACTION_ID,projectContext);
	cmdDebug->setAttribute(Core::Command::CA_Hide);
	cmdRel->setAttribute(Core::Command::CA_Hide);
	cmdMin->setAttribute(Core::Command::CA_Hide);
	cmdReldeb->setAttribute(Core::Command::CA_Hide);

	m_mapper = new QSignalMapper(this);
	connect(m_mapper, SIGNAL(mapped(const QString &)), this, SLOT(runCMake(const QString &)));

	connect(cmdDebug->action(), SIGNAL(triggered(bool)), m_mapper, SLOT(map()));
	connect(cmdRel->action(), SIGNAL(triggered(bool)), m_mapper, SLOT(map()));
	connect(cmdMin->action(), SIGNAL(triggered(bool)), m_mapper, SLOT(map()));
	connect(cmdReldeb->action(), SIGNAL(triggered(bool)), m_mapper, SLOT(map()));
	m_mapper->setMapping(cmdDebug->action(), "Debug");
	m_mapper->setMapping(cmdRel->action(), "Release");
	m_mapper->setMapping(cmdMin->action(), "Minsizerel");
	m_mapper->setMapping(cmdReldeb->action(), "Relwithdebinfo");

	menu->addAction(cmdDebug);
	menu->addAction(cmdRel);
	menu->addAction(cmdMin);
	menu->addAction(cmdReldeb);

    m_contextProject = NULL;
}

void CMakeManager::updateContextMenu(ProjectExplorer::Project *project, ProjectExplorer::Node *node)
{
    Q_UNUSED(node);
    m_contextProject = project;
}

void CMakeManager::runCMake()
{
    runCMake(ProjectExplorer::ProjectExplorerPlugin::currentProject());
}

void CMakeManager::runCMakeContextMenu()
{
    runCMake(m_contextProject);
}

void CMakeManager::runCMake(const QString & build)
{
	runCMake(m_contextProject, build);
}

void CMakeManager::runCMake(ProjectExplorer::Project *project, const QString & build)
{
	if (!project)
		return;
	CMakeProject *cmakeProject = qobject_cast<CMakeProject *>(project);
	if (!cmakeProject || !cmakeProject->activeTarget() || !cmakeProject->activeTarget()->activeBuildConfiguration())
		return;

	CMakeBuildConfiguration *bc
			= static_cast<CMakeBuildConfiguration *>(cmakeProject->activeTarget()->activeBuildConfiguration());

	if(build.length())
		bc->setBuildType(build);

	CMakeOpenProjectWizard copw(this, CMakeOpenProjectWizard::WantToUpdate,
								CMakeOpenProjectWizard::BuildInfo(bc));

	if (copw.exec() == QDialog::Accepted)
		cmakeProject->parseCMakeLists();
}

ProjectExplorer::Project *CMakeManager::openProject(const QString &fileName, QString *errorString)
{
    Q_UNUSED(errorString)
	bool canOpen = true;
	QList<ProjectExplorer::Project *> projects = ProjectExplorer::ProjectExplorerPlugin::instance()->session()->projects();
	foreach(ProjectExplorer::Project *p, projects)
	{
		if(p->files( ProjectExplorer::Project::ExcludeGeneratedFiles ).contains(fileName))
		{
			canOpen = false;
			break;
		}
	}
	if(canOpen)
		return new CMakeProject(this, fileName);
	else
	{
		QMessageBox::critical(NULL, tr("Failed to open project"), tr("Project %1 is already opened").arg(fileName), QMessageBox::Ok);
		return NULL;
	}
}

QString CMakeManager::mimeType() const
{
    return Constants::CMAKEMIMETYPE;
}

QString CMakeManager::cmakeExecutable() const
{
    return m_settingsPage->cmakeExecutable();
}

bool CMakeManager::isCMakeExecutableValid() const
{
    return m_settingsPage->isCMakeExecutableValid();
}

void CMakeManager::setCMakeExecutable(const QString &executable)
{
    m_settingsPage->setCMakeExecutable(executable);
}

bool CMakeManager::hasCodeBlocksMsvcGenerator() const
{
    return m_settingsPage->hasCodeBlocksMsvcGenerator();
}

bool CMakeManager::hasCodeBlocksNinjaGenerator() const
{
    return m_settingsPage->hasCodeBlocksNinjaGenerator();
}

ProjectExplorer::Project* CMakeManager::currentProject()
{
    return m_contextProject;
}

CMakeSettingsPage *CMakeManager::settingsPage()
{
    return m_settingsPage;
}

// need to refactor this out
// we probably want the process instead of this function
// cmakeproject then could even run the cmake process in the background, adding the files afterwards
// sounds like a plan
void CMakeManager::createXmlFile(Utils::QtcProcess *proc, const QString &arguments,
                                 const QString &sourceDirectory, const QDir &buildDirectory,
                                 const Utils::Environment &env, const QString &generator)
{
    // We create a cbp file, only if we didn't find a cbp file in the base directory
    // Yet that can still override cbp files in subdirectories
    // And we are creating tons of files in the source directories
    // All of that is not really nice.
    // The mid term plan is to move away from the CodeBlocks Generator and use our own
    // QtCreator generator, which actually can be very similar to the CodeBlock Generator
    QString buildDirectoryPath = buildDirectory.absolutePath();
    buildDirectory.mkpath(buildDirectoryPath);
    proc->setWorkingDirectory(buildDirectoryPath);
    proc->setEnvironment(env);

    const QString srcdir = buildDirectory.exists(QLatin1String("CMakeCache.txt")) ?
                QString(QLatin1Char('.')) : sourceDirectory;
    QString args;
    Utils::QtcProcess::addArg(&args, srcdir);
    Utils::QtcProcess::addArgs(&args, arguments);
    Utils::QtcProcess::addArg(&args, generator);
    proc->setCommand(cmakeExecutable(), args);
    proc->start();
}

QString CMakeManager::findCbpFile(const QDir &directory)
{
    // Find the cbp file
    //   the cbp file is named like the project() command in the CMakeList.txt file
    //   so this method below could find the wrong cbp file, if the user changes the project()
    //   2name
    QDateTime t;
    QString file;
    foreach (const QString &cbpFile , directory.entryList()) {
        if (cbpFile.endsWith(QLatin1String(".cbp"))) {
            QFileInfo fi(directory.path() + QLatin1Char('/') + cbpFile);
            if (t.isNull() || fi.lastModified() > t) {
                file = directory.path() + QLatin1Char('/') + cbpFile;
                t = fi.lastModified();
            }
        }
    }
    return file;
}

// This code is duplicated from qtversionmanager
QString CMakeManager::qtVersionForQMake(const QString &qmakePath)
{
    QProcess qmake;
    qmake.start(qmakePath, QStringList(QLatin1String("--version")));
    if (!qmake.waitForStarted()) {
        qWarning("Cannot start '%s': %s", qPrintable(qmakePath), qPrintable(qmake.errorString()));
        return QString();
    }
    if (!qmake.waitForFinished())      {
        Utils::SynchronousProcess::stopProcess(qmake);
        qWarning("Timeout running '%s'.", qPrintable(qmakePath));
        return QString();
    }
    QString output = qmake.readAllStandardOutput();
    QRegExp regexp(QLatin1String("(QMake version|Qmake version:)[\\s]*([\\d.]*)"));
    regexp.indexIn(output);
    if (regexp.cap(2).startsWith(QLatin1String("2."))) {
        QRegExp regexp2(QLatin1String("Using Qt version[\\s]*([\\d\\.]*)"));
        regexp2.indexIn(output);
        return regexp2.cap(1);
    }
    return QString();
}

/////
// CMakeSettingsPage
////


CMakeSettingsPage::CMakeSettingsPage()
    :  m_pathchooser(0)
{
    setId(QLatin1String("Z.CMake"));
    setDisplayName(tr("CMake"));
    setCategory(QLatin1String(ProjectExplorer::Constants::PROJECTEXPLORER_SETTINGS_CATEGORY));
    setDisplayCategory(QCoreApplication::translate("ProjectExplorer",
       ProjectExplorer::Constants::PROJECTEXPLORER_SETTINGS_TR_CATEGORY));
    setCategoryIcon(QLatin1String(ProjectExplorer::Constants::PROJECTEXPLORER_SETTINGS_CATEGORY_ICON));

    m_userCmake.process = 0;
    m_pathCmake.process = 0;
    m_userCmake.hasCodeBlocksMsvcGenerator = false;
    m_pathCmake.hasCodeBlocksMsvcGenerator = false;
    m_userCmake.hasCodeBlocksNinjaGenerator = false;
    m_pathCmake.hasCodeBlocksNinjaGenerator = false;
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(QLatin1String("CMakeSettings"));
    m_userCmake.executable = settings->value(QLatin1String("cmakeExecutable")).toString();

	settings->beginGroup("CMakeProjects");

	foreach(QString id, settings->allKeys())
	{
		QVariantMap map;
		map = settings->value( id ).toMap();
		m_cmakeProperties.insert(id, map);
	}

	settings->endGroup();
    settings->endGroup();

    updateInfo(&m_userCmake);
    m_pathCmake.executable = findCmakeExecutable();
    updateInfo(&m_pathCmake);
}

void CMakeSettingsPage::startProcess(CMakeValidator *cmakeValidator)
{
    cmakeValidator->process = new QProcess();

    if (cmakeValidator == &m_pathCmake) // ugly
        connect(cmakeValidator->process, SIGNAL(finished(int)),
                this, SLOT(userCmakeFinished()));
    else
        connect(cmakeValidator->process, SIGNAL(finished(int)),
                this, SLOT(pathCmakeFinished()));

    cmakeValidator->process->start(cmakeValidator->executable, QStringList(QLatin1String("--help")));
    cmakeValidator->process->waitForStarted();
}

void CMakeSettingsPage::userCmakeFinished()
{
    cmakeFinished(&m_userCmake);
}

void CMakeSettingsPage::pathCmakeFinished()
{
    cmakeFinished(&m_pathCmake);
}

void CMakeSettingsPage::addProperty()
{
	m_tableWidget->insertRow( m_tableWidget->currentRow() + 1 );
}

void CMakeSettingsPage::deleteProperty()
{
	int row = m_tableWidget->currentRow();

	QTableWidgetItem *first = m_tableWidget->item(row, 0);

	if(first)
	{
		if(QMessageBox::question(NULL, tr("Delete project property"),
                                 tr("Do you really want to delete %1 property %2 ?").arg(m_currentProject).arg(first->text()),
								 QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok)
		{
			m_tableWidget->removeRow( row );
		}
	}
	else
		m_tableWidget->removeRow( row );

}

void CMakeSettingsPage::cmakeFinished(CMakeValidator *cmakeValidator) const
{
    if (cmakeValidator->process) {
        cmakeValidator->process->waitForFinished();
        QString response = cmakeValidator->process->readAll();
        QRegExp versionRegexp(QLatin1String("^cmake version ([\\d\\.]*)"));
        versionRegexp.indexIn(response);

        //m_supportsQtCreator = response.contains(QLatin1String("QtCreator"));
        cmakeValidator->hasCodeBlocksNinjaGenerator = response.contains(QLatin1String("CodeBlocks - Ninja"));
        cmakeValidator->hasCodeBlocksMsvcGenerator = response.contains(QLatin1String("CodeBlocks - NMake Makefiles"));
        cmakeValidator->version = versionRegexp.cap(1);
        if (!(versionRegexp.capturedTexts().size() > 3))
            cmakeValidator->version += QLatin1Char('.') + versionRegexp.cap(3);

        if (cmakeValidator->version.isEmpty())
            cmakeValidator->state = CMakeValidator::INVALID;
        else
            cmakeValidator->state = CMakeValidator::VALID;

        cmakeValidator->process->deleteLater();
        cmakeValidator->process = 0;
    }
}

bool CMakeSettingsPage::isCMakeExecutableValid() const
{
    if (m_userCmake.state == CMakeValidator::RUNNING) {
        disconnect(m_userCmake.process, SIGNAL(finished(int)),
                   this, SLOT(userCmakeFinished()));
        m_userCmake.process->waitForFinished();
        // Parse the output now
        cmakeFinished(&m_userCmake);
    }

    if (m_userCmake.state == CMakeValidator::VALID)
        return true;
    if (m_pathCmake.state == CMakeValidator::RUNNING) {
        disconnect(m_userCmake.process, SIGNAL(finished(int)),
                   this, SLOT(pathCmakeFinished()));
        m_pathCmake.process->waitForFinished();
        // Parse the output now
        cmakeFinished(&m_pathCmake);
    }
    return m_pathCmake.state == CMakeValidator::VALID;
}

CMakeSettingsPage::~CMakeSettingsPage()
{
    if (m_userCmake.process)
        m_userCmake.process->waitForFinished();
    delete m_userCmake.process;
    if (m_pathCmake.process)
        m_pathCmake.process->waitForFinished();
	delete m_pathCmake.process;

	delete m_tableWidget;
	delete m_projects;
	delete m_addProperty;
	delete m_deleteProperty;
}

QString CMakeSettingsPage::findCmakeExecutable() const
{
    Utils::Environment env = Utils::Environment::systemEnvironment();
    return env.searchInPath(QLatin1String("cmake"));
}

QWidget *CMakeSettingsPage::createPage(QWidget *parent)
{
    QWidget *outerWidget = new QWidget(parent);
    QFormLayout *formLayout = new QFormLayout(outerWidget);
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_pathchooser = new Utils::PathChooser;
    m_pathchooser->setExpectedKind(Utils::PathChooser::ExistingCommand);
    formLayout->addRow(tr("Executable:"), m_pathchooser);
    formLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Ignored, QSizePolicy::MinimumExpanding));

	QHBoxLayout *hbox = new QHBoxLayout(outerWidget);
	QVBoxLayout *vbox = new QVBoxLayout(outerWidget);

	m_tableWidget = new QTableWidget();
	m_tableWidget->setColumnCount(2);
	m_tableWidget->horizontalHeader()->setResizeMode(QHeaderView::Stretch);
	m_tableWidget->setHorizontalHeaderLabels( QStringList() << tr("Property") << tr("Value") );

	m_addProperty = new QPushButton(tr("Add"));
	m_addProperty->setMaximumWidth(100);
	connect(m_addProperty, SIGNAL(clicked()), this, SLOT(addProperty()));
	m_deleteProperty = new QPushButton(tr("Delete"));
	m_deleteProperty->setMaximumWidth(100);
	connect(m_deleteProperty, SIGNAL(clicked()), this, SLOT(deleteProperty()));

	m_projects = new QComboBox();
	QList<ProjectExplorer::Project *> projects = ProjectExplorer::ProjectExplorerPlugin::instance()->session()->projects();
	foreach(ProjectExplorer::Project *p, projects)
	{
		CMakeProject* cp = qobject_cast<CMakeProject*>(p);
		if(cp)
			m_projects->addItem( p->displayName(), p->displayName() );
	}
	connect(m_projects, SIGNAL(currentIndexChanged(QString)), this, SLOT(projectChanged(QString)));
	if(!m_projects->count())
	{
		m_currentProject = "";
		m_addProperty->setEnabled(false);
		m_deleteProperty->setEnabled(false);
		m_tableWidget->setEnabled(false);
		m_projects->setEnabled(false);
	}
	else
	{
        m_currentProject = "";
        m_projects->setCurrentIndex( 0 );
        projectChanged( m_projects->itemData( 0 ).toString() );
	}

	hbox->addWidget(m_tableWidget);
	vbox->addWidget(m_addProperty);
	vbox->addWidget(m_deleteProperty);
	vbox->addWidget(m_projects);
	hbox->addLayout(vbox);
	formLayout->addRow(tr("Arguments:"), hbox);

    m_pathchooser->setPath(m_userCmake.executable);
    return outerWidget;
}

void CMakeSettingsPage::updateInfo(CMakeValidator *cmakeValidator)
{
    QFileInfo fi(cmakeValidator->executable);
    if (fi.exists() && fi.isExecutable()) {
        // Run it to find out more
        cmakeValidator->state = CMakeValidator::RUNNING;
        startProcess(cmakeValidator);
    } else {
        cmakeValidator->state = CMakeValidator::INVALID;
    }
    saveSettings();
}

void CMakeSettingsPage::saveSettings() const
{
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(QLatin1String("CMakeSettings"));
    settings->setValue(QLatin1String("cmakeExecutable"), m_userCmake.executable);
	settings->beginGroup("CMakeProjects");
	QHashIterator<QString, QVariantMap> it(m_cmakeProperties);
	while(it.hasNext())
	{
		it.next();
		settings->setValue( it.key(), it.value() );
	}
	settings->endGroup();
    settings->endGroup();
}

void CMakeSettingsPage::saveProjectSettings(const QString & project)
{
	if(project.length() > 0)
	{
		QVariantMap map;

        for(int i = 0; i < m_tableWidget->rowCount(); i++)
		{
            QTableWidgetItem *first = m_tableWidget->item( i, 0 );
            QTableWidgetItem *second = m_tableWidget->item( i, 1 );

			if(first && second)
                map.insert(first->text(), second->text());
		}

		m_cmakeProperties.insert(project, map);
	}
}

void CMakeSettingsPage::projectChanged(QString project)
{
    if(m_currentProject.length())
	{
		// Save previous settings
		saveProjectSettings(m_currentProject);
	}

    m_tableWidget->clear();
    for(int i = 0; i < m_tableWidget->rowCount(); i++)
        m_tableWidget->removeRow( i );
    m_tableWidget->setHorizontalHeaderLabels( QStringList() << tr("Property") << tr("Value") );

    QMap<QString, QVariant> properties = m_cmakeProperties.value(project);
    if(properties.size())
    {
        QMapIterator<QString, QVariant> it(properties);
        while(it.hasNext())
        {
            it.next();

            int row = m_tableWidget->currentRow() + 1;
            m_tableWidget->insertRow( row );
            QTableWidgetItem *first = new QTableWidgetItem( it.key() );
            QTableWidgetItem *second = new QTableWidgetItem( it.value().toString() );
            m_tableWidget->setItem( row, 0, first );
            m_tableWidget->setItem( row, 1, second );
        }
    }

    m_currentProject = project;
}

QMap<QString,QVariant> CMakeSettingsPage::getArguments(const QString & project) const
{
    return m_cmakeProperties.value(project);
}

void CMakeSettingsPage::apply()
{
    saveProjectSettings(m_currentProject);
	saveSettings();
    if (!m_pathchooser) // page was never shown
        return;
    if (m_userCmake.executable == m_pathchooser->path())
        return;
    m_userCmake.executable = m_pathchooser->path();
    updateInfo(&m_userCmake);
}

void CMakeSettingsPage::finish()
{
	saveProjectSettings(m_currentProject);
}

QString CMakeSettingsPage::cmakeExecutable() const
{
    if (!isCMakeExecutableValid())
        return QString();
    if (m_userCmake.state == CMakeValidator::VALID)
        return m_userCmake.executable;
    else
        return m_pathCmake.executable;
}

void CMakeSettingsPage::setCMakeExecutable(const QString &executable)
{
    if (m_userCmake.executable == executable)
        return;
    m_userCmake.executable = executable;
    updateInfo(&m_userCmake);
}

bool CMakeSettingsPage::hasCodeBlocksMsvcGenerator() const
{
    if (!isCMakeExecutableValid())
        return false;
    if (m_userCmake.state == CMakeValidator::VALID)
        return m_userCmake.hasCodeBlocksMsvcGenerator;
    else
        return m_pathCmake.hasCodeBlocksMsvcGenerator;
}

bool CMakeSettingsPage::hasCodeBlocksNinjaGenerator() const
{
    if (!isCMakeExecutableValid())
        return false;
    if (m_userCmake.state == CMakeValidator::VALID)
        return m_userCmake.hasCodeBlocksNinjaGenerator;
    else
        return m_pathCmake.hasCodeBlocksNinjaGenerator;
}
