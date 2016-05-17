#include "worldspacewidget.hpp"

#include <algorithm>
#include <iostream>

#include <QEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QToolTip>

#include <osgUtil/LineSegmentIntersector>

#include "../../model/world/universalid.hpp"
#include "../../model/world/idtable.hpp"

#include "../../model/prefs/state.hpp"

#include "../render/orbitcameramode.hpp"

#include "../widget/scenetoolmode.hpp"
#include "../widget/scenetooltoggle2.hpp"
#include "../widget/scenetoolrun.hpp"

#include "object.hpp"
#include "mask.hpp"
#include "instancemode.hpp"
#include "pathgridmode.hpp"
#include "cameracontroller.hpp"

CSVRender::WorldspaceWidget::WorldspaceWidget (CSMDoc::Document& document, QWidget* parent)
: SceneWidget (document.getData().getResourceSystem(), parent, 0, false), mSceneElements(0), mRun(0), mDocument(document),
  mInteractionMask (0), mEditMode (0), mLocked (false), mDragging (false), mDragX(0), mDragY(0), mDragFactor(0),
  mDragWheelFactor(0), mDragShiftFactor(0),
  mToolTipPos (-1, -1), mShowToolTips(false), mToolTipDelay(0)
{
    setAcceptDrops(true);

    QAbstractItemModel *referenceables =
        document.getData().getTableModel (CSMWorld::UniversalId::Type_Referenceables);

    connect (referenceables, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
        this, SLOT (referenceableDataChanged (const QModelIndex&, const QModelIndex&)));
    connect (referenceables, SIGNAL (rowsAboutToBeRemoved (const QModelIndex&, int, int)),
        this, SLOT (referenceableAboutToBeRemoved (const QModelIndex&, int, int)));
    connect (referenceables, SIGNAL (rowsInserted (const QModelIndex&, int, int)),
        this, SLOT (referenceableAdded (const QModelIndex&, int, int)));

    QAbstractItemModel *references =
        document.getData().getTableModel (CSMWorld::UniversalId::Type_References);

    connect (references, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
        this, SLOT (referenceDataChanged (const QModelIndex&, const QModelIndex&)));
    connect (references, SIGNAL (rowsAboutToBeRemoved (const QModelIndex&, int, int)),
        this, SLOT (referenceAboutToBeRemoved (const QModelIndex&, int, int)));
    connect (references, SIGNAL (rowsInserted (const QModelIndex&, int, int)),
        this, SLOT (referenceAdded (const QModelIndex&, int, int)));

    QAbstractItemModel *pathgrids = document.getData().getTableModel (CSMWorld::UniversalId::Type_Pathgrids);

    connect (pathgrids, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
        this, SLOT (pathgridDataChanged (const QModelIndex&, const QModelIndex&)));
    connect (pathgrids, SIGNAL (rowsAboutToBeRemoved (const QModelIndex&, int, int)),
        this, SLOT (pathgridAboutToBeRemoved (const QModelIndex&, int, int)));
    connect (pathgrids, SIGNAL (rowsInserted (const QModelIndex&, int, int)),
        this, SLOT (pathgridAdded (const QModelIndex&, int, int)));

    QAbstractItemModel *debugProfiles =
        document.getData().getTableModel (CSMWorld::UniversalId::Type_DebugProfiles);

    connect (debugProfiles, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
        this, SLOT (debugProfileDataChanged (const QModelIndex&, const QModelIndex&)));
    connect (debugProfiles, SIGNAL (rowsAboutToBeRemoved (const QModelIndex&, int, int)),
        this, SLOT (debugProfileAboutToBeRemoved (const QModelIndex&, int, int)));

    mToolTipDelayTimer.setSingleShot (true);
    connect (&mToolTipDelayTimer, SIGNAL (timeout()), this, SLOT (showToolTip()));

    CSMPrefs::get()["3D Scene Input"].update();
    CSMPrefs::get()["Tooltips"].update();
}

CSVRender::WorldspaceWidget::~WorldspaceWidget ()
{
}

void CSVRender::WorldspaceWidget::settingChanged (const CSMPrefs::Setting *setting)
{
    if (*setting=="3D Scene Input/drag-factor")
        mDragFactor = setting->toDouble();
    else if (*setting=="3D Scene Input/drag-wheel-factor")
        mDragWheelFactor = setting->toDouble();
    else if (*setting=="3D Scene Input/drag-shift-factor")
        mDragShiftFactor = setting->toDouble();
    else if (*setting=="Tooltips/scene-delay")
        mToolTipDelay = setting->toInt();
    else if (*setting=="Tooltips/scene")
        mShowToolTips = setting->isTrue();
    else
        SceneWidget::settingChanged(setting);
}


void CSVRender::WorldspaceWidget::useViewHint (const std::string& hint) {}

void CSVRender::WorldspaceWidget::selectDefaultNavigationMode()
{
    selectNavigationMode("1st");
}

void CSVRender::WorldspaceWidget::centerOrbitCameraOnSelection()
{
    std::vector<osg::ref_ptr<TagBase> > selection = getSelection(~0);

    for (std::vector<osg::ref_ptr<TagBase> >::iterator it = selection.begin(); it!=selection.end(); ++it)
    {
        if (CSVRender::ObjectTag *objectTag = dynamic_cast<CSVRender::ObjectTag*> (it->get()))
        {
            mOrbitCamControl->setCenter(objectTag->mObject->getPosition().asVec3());
        }
    }
}

CSVWidget::SceneToolMode *CSVRender::WorldspaceWidget::makeNavigationSelector (
    CSVWidget::SceneToolbar *parent)
{
    CSVWidget::SceneToolMode *tool = new CSVWidget::SceneToolMode (parent, "Camera Mode");

    /// \todo replace icons
    /// \todo consider user-defined button-mapping
    tool->addButton (":scenetoolbar/1st-person", "1st",
        "First Person"
        "<ul><li>Mouse-Look while holding the left button</li>"
        "<li>WASD movement keys</li>"
        "<li>Mouse wheel moves the camera forward/backward</li>"
        "<li>Strafing (also vertically) by holding the left mouse button and control</li>"
        "<li>Camera is held upright</li>"
        "<li>Hold shift to speed up movement</li>"
        "</ul>");
    tool->addButton (":scenetoolbar/free-camera", "free",
        "Free Camera"
        "<ul><li>Mouse-Look while holding the left button</li>"
        "<li>Strafing (also vertically) via WASD or by holding the left mouse button and control</li>"
        "<li>Mouse wheel moves the camera forward/backward</li>"
        "<li>Roll camera with Q and E keys</li>"
        "<li>Hold shift to speed up movement</li>"
        "</ul>");
    tool->addButton(
        new CSVRender::OrbitCameraMode(this, QIcon(":scenetoolbar/orbiting-camera"),
            "Orbiting Camera"
            "<ul><li>Always facing the centre point</li>"
            "<li>Rotate around the centre point via WASD or by moving the mouse while holding the left button</li>"
            "<li>Mouse wheel moves camera away or towards centre point but can not pass through it</li>"
            "<li>Roll camera with Q and E keys</li>"
            "<li>Strafing (also vertically) by holding the left mouse button and control (includes relocation of the centre point)</li>"
            "<li>Hold shift to speed up movement</li>"
            "</ul>", tool),
        "orbit");

    connect (tool, SIGNAL (modeChanged (const std::string&)),
        this, SLOT (selectNavigationMode (const std::string&)));

    return tool;
}

CSVWidget::SceneToolToggle2 *CSVRender::WorldspaceWidget::makeSceneVisibilitySelector (CSVWidget::SceneToolbar *parent)
{
    mSceneElements = new CSVWidget::SceneToolToggle2 (parent,
        "Scene Element Visibility", ":scenetoolbar/scene-view-c", ":scenetoolbar/scene-view-");

    addVisibilitySelectorButtons (mSceneElements);

    mSceneElements->setSelectionMask (0xffffffff);

    connect (mSceneElements, SIGNAL (selectionChanged()),
        this, SLOT (elementSelectionChanged()));

    return mSceneElements;
}

CSVWidget::SceneToolRun *CSVRender::WorldspaceWidget::makeRunTool (
    CSVWidget::SceneToolbar *parent)
{
    CSMWorld::IdTable& debugProfiles = dynamic_cast<CSMWorld::IdTable&> (
        *mDocument.getData().getTableModel (CSMWorld::UniversalId::Type_DebugProfiles));

    std::vector<std::string> profiles;

    int idColumn = debugProfiles.findColumnIndex (CSMWorld::Columns::ColumnId_Id);
    int stateColumn = debugProfiles.findColumnIndex (CSMWorld::Columns::ColumnId_Modification);
    int defaultColumn = debugProfiles.findColumnIndex (
        CSMWorld::Columns::ColumnId_DefaultProfile);

    int size = debugProfiles.rowCount();

    for (int i=0; i<size; ++i)
    {
        int state = debugProfiles.data (debugProfiles.index (i, stateColumn)).toInt();

        bool default_ = debugProfiles.data (debugProfiles.index (i, defaultColumn)).toInt();

        if (state!=CSMWorld::RecordBase::State_Deleted && default_)
            profiles.push_back (
                debugProfiles.data (debugProfiles.index (i, idColumn)).
                toString().toUtf8().constData());
    }

    std::sort (profiles.begin(), profiles.end());

    mRun = new CSVWidget::SceneToolRun (parent, "Run OpenMW from the current camera position",
        ":scenetoolbar/play", profiles);

    connect (mRun, SIGNAL (runRequest (const std::string&)),
        this, SLOT (runRequest (const std::string&)));

    return mRun;
}

CSVWidget::SceneToolMode *CSVRender::WorldspaceWidget::makeEditModeSelector (
    CSVWidget::SceneToolbar *parent)
{
    mEditMode = new CSVWidget::SceneToolMode (parent, "Edit Mode");

    addEditModeSelectorButtons (mEditMode);

    connect (mEditMode, SIGNAL (modeChanged (const std::string&)),
        this, SLOT (editModeChanged (const std::string&)));

    return mEditMode;
}

CSVRender::WorldspaceWidget::DropType CSVRender::WorldspaceWidget::getDropType (
    const std::vector< CSMWorld::UniversalId >& data)
{
    DropType output = Type_Other;

    for (std::vector<CSMWorld::UniversalId>::const_iterator iter (data.begin());
        iter!=data.end(); ++iter)
    {
        DropType type = Type_Other;

        if (iter->getType()==CSMWorld::UniversalId::Type_Cell ||
            iter->getType()==CSMWorld::UniversalId::Type_Cell_Missing)
        {
            type = iter->getId().substr (0, 1)=="#" ? Type_CellsExterior : Type_CellsInterior;
        }
        else if (iter->getType()==CSMWorld::UniversalId::Type_DebugProfile)
            type = Type_DebugProfile;

        if (iter==data.begin())
            output = type;
        else if  (output!=type) // mixed types -> ignore
            return Type_Other;
    }

    return output;
}

CSVRender::WorldspaceWidget::dropRequirments
    CSVRender::WorldspaceWidget::getDropRequirements (DropType type) const
{
    if (type==Type_DebugProfile)
        return canHandle;

    return ignored;
}

bool CSVRender::WorldspaceWidget::handleDrop (const std::vector<CSMWorld::UniversalId>& data,
    DropType type)
{
    if (type==Type_DebugProfile)
    {
        if (mRun)
        {
            for (std::vector<CSMWorld::UniversalId>::const_iterator iter (data.begin());
                iter!=data.end(); ++iter)
                mRun->addProfile (iter->getId());
        }

        return true;
    }

    return false;
}

unsigned int CSVRender::WorldspaceWidget::getVisibilityMask() const
{
    return mSceneElements->getSelectionMask();
}

void CSVRender::WorldspaceWidget::setInteractionMask (unsigned int mask)
{
    mInteractionMask = mask | Mask_CellMarker | Mask_CellArrow;
}

unsigned int CSVRender::WorldspaceWidget::getInteractionMask() const
{
    return mInteractionMask & getVisibilityMask();
}

void CSVRender::WorldspaceWidget::setEditLock (bool locked)
{
    dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent()).setEditLock (locked);
}

void CSVRender::WorldspaceWidget::addVisibilitySelectorButtons (
    CSVWidget::SceneToolToggle2 *tool)
{
    tool->addButton (Button_Reference, Mask_Reference, "Instances");
    tool->addButton (Button_Water, Mask_Water, "Water");
    tool->addButton (Button_Pathgrid, Mask_Pathgrid, "Pathgrid");
}

void CSVRender::WorldspaceWidget::addEditModeSelectorButtons (CSVWidget::SceneToolMode *tool)
{
    /// \todo replace EditMode with suitable subclasses
    tool->addButton (new InstanceMode (this, tool), "object");
    tool->addButton (new PathgridMode (this, tool), "pathgrid");
}

CSMDoc::Document& CSVRender::WorldspaceWidget::getDocument()
{
    return mDocument;
}

osg::Vec3f CSVRender::WorldspaceWidget::getIntersectionPoint (const QPoint& localPos,
    unsigned int interactionMask, bool ignoreHidden) const
{
    // (0,0) is considered the lower left corner of an OpenGL window
    int x = localPos.x();
    int y = height() - localPos.y();

    osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector (
        new osgUtil::LineSegmentIntersector (osgUtil::Intersector::WINDOW, x, y));

    intersector->setIntersectionLimit (osgUtil::LineSegmentIntersector::NO_LIMIT);
    osgUtil::IntersectionVisitor visitor (intersector);

    unsigned int mask = interactionMask;

    if (ignoreHidden)
        mask &= getVisibilityMask();

    visitor.setTraversalMask (mask);

    mView->getCamera()->accept (visitor);

    for (osgUtil::LineSegmentIntersector::Intersections::iterator iter = intersector->getIntersections().begin();
         iter!=intersector->getIntersections().end(); ++iter)
    {
        // reject back-facing polygons
        osg::Vec3f normal = osg::Matrix::transform3x3 (
            iter->getWorldIntersectNormal(), mView->getCamera()->getViewMatrix());

        if (normal.z()>=0)
            return iter->getWorldIntersectPoint();
    }

    osg::Matrixd matrix;
    matrix.preMult (mView->getCamera()->getViewport()->computeWindowMatrix());
    matrix.preMult (mView->getCamera()->getProjectionMatrix());
    matrix.preMult (mView->getCamera()->getViewMatrix());
    matrix = osg::Matrixd::inverse (matrix);

    osg::Vec3d start = matrix.preMult (intersector->getStart());
    osg::Vec3d end = matrix.preMult (intersector->getEnd());

    osg::Vec3d direction = end-start;
    direction.normalize();

    return start + direction * CSMPrefs::get()["Scene Drops"]["distance"].toInt();
}

void CSVRender::WorldspaceWidget::abortDrag()
{
    if (mDragging)
    {
        EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());

        editMode.dragAborted();
        mDragging = false;
        mDragMode.clear();
    }
}

void CSVRender::WorldspaceWidget::dragEnterEvent (QDragEnterEvent* event)
{
    const CSMWorld::TableMimeData* mime = dynamic_cast<const CSMWorld::TableMimeData*> (event->mimeData());
    if (!mime) // May happen when non-records (e.g. plain text) are dragged and dropped
        return;

    if (mime->fromDocument (mDocument))
    {
        if (mime->holdsType (CSMWorld::UniversalId::Type_Cell) ||
            mime->holdsType (CSMWorld::UniversalId::Type_Cell_Missing) ||
            mime->holdsType (CSMWorld::UniversalId::Type_DebugProfile))
        {
            // These drops are handled through the subview object.
            event->accept();
        }
        else
            dynamic_cast<EditMode&> (*mEditMode->getCurrent()).dragEnterEvent (event);
    }
}

void CSVRender::WorldspaceWidget::dragMoveEvent(QDragMoveEvent *event)
{
    const CSMWorld::TableMimeData* mime = dynamic_cast<const CSMWorld::TableMimeData*> (event->mimeData());
    if (!mime) // May happen when non-records (e.g. plain text) are dragged and dropped
        return;

    if (mime->fromDocument (mDocument))
    {
        if (mime->holdsType (CSMWorld::UniversalId::Type_Cell) ||
            mime->holdsType (CSMWorld::UniversalId::Type_Cell_Missing) ||
            mime->holdsType (CSMWorld::UniversalId::Type_DebugProfile))
        {
            // These drops are handled through the subview object.
            event->accept();
        }
        else
            dynamic_cast<EditMode&> (*mEditMode->getCurrent()).dragMoveEvent (event);
    }
}

bool CSVRender::WorldspaceWidget::storeMappingSetting (const CSMPrefs::Setting *setting)
{
    static const char * const sMappingSettings[] =
    {
        "p-edit", "s-edit",
        "p-select", "s-select",
        0
    };

    if (setting->getParent()->getKey()=="3D Scene Input")
    {
        for (int i=0; sMappingSettings[i]; ++i)
        {
            if (setting->getKey()==sMappingSettings[i])
            {
                QString value = QString::fromUtf8 (setting->toString().c_str());

                Qt::MouseButton button = Qt::NoButton;

                if (value.endsWith ("Left Mouse-Button"))
                    button = Qt::LeftButton;
                else if (value.endsWith ("Right Mouse-Button"))
                    button = Qt::RightButton;
                else if (value.endsWith ("Middle Mouse-Button"))
                    button = Qt::MiddleButton;
                else
                    return false;

                bool ctrl = value.startsWith ("Ctrl-");

                mButtonMapping[std::make_pair (button, ctrl)] = sMappingSettings[i];
                return true;
            }
        }
    }

    return SceneWidget::storeMappingSetting(setting);
}

CSVRender::WorldspaceHitResult CSVRender::WorldspaceWidget::mousePick (const QPoint& localPos)
{
    // (0,0) is considered the lower left corner of an OpenGL window
    int x = localPos.x();
    int y = height() - localPos.y();

    osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector (new osgUtil::LineSegmentIntersector(osgUtil::Intersector::WINDOW, x, y));

    intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::NO_LIMIT);
    osgUtil::IntersectionVisitor visitor(intersector);

    visitor.setTraversalMask(getInteractionMask());

    mView->getCamera()->accept(visitor);

    for (osgUtil::LineSegmentIntersector::Intersections::iterator it = intersector->getIntersections().begin();
         it != intersector->getIntersections().end(); ++it)
    {
        osgUtil::LineSegmentIntersector::Intersection intersection = *it;

        // reject back-facing polygons
        osg::Vec3f normal = intersection.getWorldIntersectNormal();
        normal = osg::Matrix::transform3x3(normal, mView->getCamera()->getViewMatrix());
        if (normal.z() < 0)
            continue;

        for (std::vector<osg::Node*>::iterator it = intersection.nodePath.begin(); it != intersection.nodePath.end(); ++it)
        {
            osg::Node* node = *it;
            if (osg::ref_ptr<CSVRender::TagBase> tag = dynamic_cast<CSVRender::TagBase *>(node->getUserData()))
            {
                WorldspaceHitResult hit = { true, tag, 0, 0, 0, intersection.getWorldIntersectPoint() };
                if (intersection.indexList.size() >= 3)
                {
                    hit.index0 = intersection.indexList[0];
                    hit.index1 = intersection.indexList[1];
                    hit.index2 = intersection.indexList[2];
                }
                return hit;
            }
        }

        // Something untagged, probably terrain
        WorldspaceHitResult hit = { true, 0, 0, 0, 0, intersection.getWorldIntersectPoint() };
        if (intersection.indexList.size() >= 3)
        {
            hit.index0 = intersection.indexList[0];
            hit.index1 = intersection.indexList[1];
            hit.index2 = intersection.indexList[2];
        }
        return hit;
    }

    WorldspaceHitResult hit = { false, 0, 0, 0, 0, osg::Vec3d() };
    return hit;
}

void CSVRender::WorldspaceWidget::dropEvent (QDropEvent* event)
{
    const CSMWorld::TableMimeData* mime = dynamic_cast<const CSMWorld::TableMimeData*> (event->mimeData());
    if (!mime) // May happen when non-records (e.g. plain text) are dragged and dropped
        return;

    if (mime->fromDocument (mDocument))
    {
        if (mime->holdsType (CSMWorld::UniversalId::Type_Cell) ||
            mime->holdsType (CSMWorld::UniversalId::Type_Cell_Missing) ||
            mime->holdsType (CSMWorld::UniversalId::Type_DebugProfile))
        {
            emit dataDropped(mime->getData());
        }
        else
            dynamic_cast<EditMode&> (*mEditMode->getCurrent()).dropEvent (event);
    }
}

void CSVRender::WorldspaceWidget::runRequest (const std::string& profile)
{
    mDocument.startRunning (profile, getStartupInstruction());
}

void CSVRender::WorldspaceWidget::debugProfileDataChanged (const QModelIndex& topLeft,
    const QModelIndex& bottomRight)
{
    if (!mRun)
        return;

    CSMWorld::IdTable& debugProfiles = dynamic_cast<CSMWorld::IdTable&> (
        *mDocument.getData().getTableModel (CSMWorld::UniversalId::Type_DebugProfiles));

    int idColumn = debugProfiles.findColumnIndex (CSMWorld::Columns::ColumnId_Id);
    int stateColumn = debugProfiles.findColumnIndex (CSMWorld::Columns::ColumnId_Modification);

    for (int i=topLeft.row(); i<=bottomRight.row(); ++i)
    {
        int state = debugProfiles.data (debugProfiles.index (i, stateColumn)).toInt();

        // As of version 0.33 this case can not happen because debug profiles exist only in
        // project or session scope, which means they will never be in deleted state. But we
        // are adding the code for the sake of completeness and to avoid surprises if debug
        // profile ever get extended to content scope.
        if (state==CSMWorld::RecordBase::State_Deleted)
            mRun->removeProfile (debugProfiles.data (
                debugProfiles.index (i, idColumn)).toString().toUtf8().constData());
    }
}

void CSVRender::WorldspaceWidget::debugProfileAboutToBeRemoved (const QModelIndex& parent,
    int start, int end)
{
    if (parent.isValid())
        return;

    if (!mRun)
        return;

    CSMWorld::IdTable& debugProfiles = dynamic_cast<CSMWorld::IdTable&> (
        *mDocument.getData().getTableModel (CSMWorld::UniversalId::Type_DebugProfiles));

    int idColumn = debugProfiles.findColumnIndex (CSMWorld::Columns::ColumnId_Id);

    for (int i=start; i<=end; ++i)
    {
        mRun->removeProfile (debugProfiles.data (
            debugProfiles.index (i, idColumn)).toString().toUtf8().constData());
    }
}

void CSVRender::WorldspaceWidget::editModeChanged (const std::string& id)
{
    dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent()).setEditLock (mLocked);
    mDragging = false;
    mDragMode.clear();
}

void CSVRender::WorldspaceWidget::showToolTip()
{
    if (mShowToolTips)
    {
        QPoint pos = QCursor::pos();

        WorldspaceHitResult hit = mousePick (mapFromGlobal (pos));
        if (hit.tag)
        {
            bool hideBasics = CSMPrefs::get()["Tooltips"]["scene-hide-basic"].isTrue();
            QToolTip::showText (pos, hit.tag->getToolTip (hideBasics), this);
        }
    }
}

void CSVRender::WorldspaceWidget::elementSelectionChanged()
{
    setVisibilityMask (getVisibilityMask());
    flagAsModified();
    updateOverlay();
}

void CSVRender::WorldspaceWidget::updateOverlay()
{
}

void CSVRender::WorldspaceWidget::mouseMoveEvent (QMouseEvent *event)
{
    if (mDragging)
    {
        int diffX = event->x() - mDragX;
        int diffY = (height() - event->y()) - mDragY;

        mDragX = event->x();
        mDragY = height() - event->y();

        double factor = mDragFactor;

        if (event->modifiers() & Qt::ShiftModifier)
            factor *= mDragShiftFactor;

        EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());

        editMode.drag (diffX, diffY, factor);
    }
    else if (mDragMode=="p-edit" || mDragMode=="s-edit" || mDragMode=="p-select" || mDragMode=="s-select")
    {
        WorldspaceHitResult hit = mousePick (event->pos());

        EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());

        if (mDragMode=="p-edit")
            mDragging = editMode.primaryEditStartDrag (hit);
        else if (mDragMode=="s-edit")
            mDragging = editMode.secondaryEditStartDrag (hit);
        else if (mDragMode=="p-select")
            mDragging = editMode.primarySelectStartDrag (hit);
        else if (mDragMode=="s-select")
            mDragging = editMode.secondarySelectStartDrag (hit);

        if (mDragging)
        {
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
            mDragX = event->localPos().x();
            mDragY = height() - event->localPos().y();
#else
            mDragX = event->posF().x();
            mDragY = height() - event->posF().y();
#endif
        }
    }
    else
    {
        if (event->globalPos()!=mToolTipPos)
        {
            mToolTipPos = event->globalPos();

            if (mShowToolTips)
                mToolTipDelayTimer.start (mToolTipDelay);
        }

        SceneWidget::mouseMoveEvent(event);
    }
}

void CSVRender::WorldspaceWidget::mousePressEvent (QMouseEvent *event)
{
    std::string button = mapButton (event);

    if (button=="p-edit" || button=="s-edit" ||
        button=="p-select" || button=="s-select")
    {
        if (!mDragging)
            mDragMode = button;
    }
    else
        SceneWidget::mousePressEvent(event);
}

void CSVRender::WorldspaceWidget::mouseReleaseEvent (QMouseEvent *event)
{
    std::string button = mapButton (event);
    mDragMode.clear();

    if (button=="p-edit" || button=="s-edit" ||
        button=="p-select" || button=="s-select")
    {
        if (mDragging)
        {
            EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());
            WorldspaceHitResult hit = mousePick (event->pos());

            editMode.dragCompleted(hit);
            mDragging = false;
        }
        else
        {
            WorldspaceHitResult hit = mousePick (event->pos());

            handleMouseClick (hit, button, event->modifiers() & Qt::ShiftModifier);
        }
    }
    else
        SceneWidget::mouseReleaseEvent(event);
}

void CSVRender::WorldspaceWidget::wheelEvent (QWheelEvent *event)
{
    if (mDragging)
    {
        double factor = mDragWheelFactor;

        if (event->modifiers() & Qt::ShiftModifier)
            factor *= mDragShiftFactor;

        EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());

        editMode.dragWheel (event->delta(), factor);
    }
    else
        SceneWidget::wheelEvent(event);
}

void CSVRender::WorldspaceWidget::keyPressEvent (QKeyEvent *event)
{
    if(event->key() == Qt::Key_Escape)
    {
        abortDrag();
    }
    else
        SceneWidget::keyPressEvent(event);
}

void CSVRender::WorldspaceWidget::handleMouseClick (const WorldspaceHitResult& hit, const std::string& button, bool shift)
{
    EditMode& editMode = dynamic_cast<CSVRender::EditMode&> (*mEditMode->getCurrent());

    if (button=="p-edit")
        editMode.primaryEditPressed (hit);
    else if (button=="s-edit")
        editMode.secondaryEditPressed (hit);
    else if (button=="p-select")
        editMode.primarySelectPressed (hit);
    else if (button=="s-select")
        editMode.secondarySelectPressed (hit);
}

CSVRender::EditMode *CSVRender::WorldspaceWidget::getEditMode()
{
    return dynamic_cast<CSVRender::EditMode *> (mEditMode->getCurrent());
}
