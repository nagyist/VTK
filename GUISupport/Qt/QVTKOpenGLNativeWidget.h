// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class QVTKOpenGLNativeWidget
 * @brief QOpenGLWidget subclass to house a vtkGenericOpenGLRenderWindow in a Qt
 * application.
 *
 * QVTKOpenGLNativeWidget extends QOpenGLWidget to make it work with a
 * vtkGenericOpenGLRenderWindow.
 *
 * Please note that QVTKOpenGLNativeWidget only works with vtkGenericOpenGLRenderWindow.
 * This is necessary since QOpenGLWidget wants to take over the window management as
 * well as the OpenGL context creation. Getting that to work reliably with
 * vtkXRenderWindow or vtkWin32RenderWindow (and other platform specific
 * vtkRenderWindow subclasses) was tricky and fraught with issues.
 *
 * Since QVTKOpenGLNativeWidget uses QOpenGLWidget to create the OpenGL context,
 * it uses QSurfaceFormat (set using `QOpenGLWidget::setFormat` or
 * `QSurfaceFormat::setDefaultFormat`) to create appropriate window and context.
 * You can use `QVTKOpenGLNativeWidget::copyToFormat` to obtain a QSurfaceFormat
 * appropriate for a vtkRenderWindow.
 *
 * A typical usage for QVTKOpenGLNativeWidget is as follows:
 * @code{.cpp}
 *
 *  // before initializing QApplication, set the default surface format.
 *  QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
 *
 *  vtkNew<vtkGenericOpenGLRenderWindow> window;
 *  QPointer<QVTKOpenGLNativeWidget> widget = new QVTKOpenGLNativeWidget(...);
 *  widget->SetRenderWindow(window.Get());
 *
 *  // If using any of the standard view e.g. vtkContextView, then
 *  // you can do the following.
 *  vtkNew<vtkContextView> view;
 *  view->SetRenderWindow(window.Get());
 *
 *  // You can continue to use `window` as a regular vtkRenderWindow
 *  // including adding renderers, actors etc.
 *
 * @endcode
 *
 * @section OpenGLContext OpenGL Context
 *
 * In QOpenGLWidget (superclass for QVTKOpenGLNativeWidget), all rendering happens in a
 * framebuffer object. Thus, care must be taken in the rendering code to never
 * directly re-bind the default framebuffer i.e. ID 0.
 *
 * QVTKOpenGLNativeWidget creates an internal QOpenGLFramebufferObject, independent of the
 * one created by superclass, for vtkRenderWindow to do the rendering in. This
 * explicit double-buffering is useful in avoiding temporary back-buffer only
 * renders done in VTK (e.g. when making selections) from destroying the results
 * composed on screen.
 *
 * @section RenderAndPaint Handling Render and Paint.
 *
 * QWidget subclasses (including `QOpenGLWidget` and `QVTKOpenGLNativeWidget`) display
 * their contents on the screen in `QWidget::paint` in response to a paint event.
 * `QOpenGLWidget` subclasses are expected to do OpenGL rendering in
 * `QOpenGLWidget::paintGL`. QWidget can receive paint events for various
 * reasons including widget getting focus/losing focus, some other widget on
 * the UI e.g. QProgressBar in status bar updating, etc.
 *
 * In VTK applications, any time the vtkRenderWindow needs to be updated to
 * render a new result, one call `vtkRenderWindow::Render` on it.
 * vtkRenderWindowInteractor set on the render window ensures that as
 * interactions happen that affect the rendered result, it calls `Render` on the
 * render window.
 *
 * Since paint in Qt can be called more often then needed, we avoid potentially
 * expensive `vtkRenderWindow::Render` calls each time that happens. Instead,
 * QVTKOpenGLNativeWidget relies on the VTK application calling
 * `vtkRenderWindow::Render` on the render window when it needs to update the
 * rendering. `paintGL` simply passes on the result rendered by the most render
 * vtkRenderWindow::Render to Qt windowing system for composing on-screen.
 *
 * There may still be occasions when we may have to render in `paint` for
 * example if the window was resized or Qt had to recreate the OpenGL context.
 * In those cases, `QVTKOpenGLNativeWidget::paintGL` can request a render by calling
 * `QVTKOpenGLNativeWidget::renderVTK`.
 *
 * @section Caveats
 * QVTKOpenGLNativeWidget does not support stereo,
 * please use QVTKOpenGLStereoWidget if you need support for stereo rendering
 *
 * QVTKOpenGLNativeWidget is targeted for Qt version 5.5 and above.
 *
 * @sa QVTKOpenGLStereoWidget QVTKRenderWidget
 *
 */
#ifndef QVTKOpenGLNativeWidget_h
#define QVTKOpenGLNativeWidget_h

#include <QOpenGLWidget>
#include <QScopedPointer> // for QScopedPointer.

#include "QVTKInteractor.h"        // needed for QVTKInteractor
#include "vtkGUISupportQtModule.h" // for export macro
#include "vtkNew.h"                // needed for vtkNew
#include "vtkSmartPointer.h"       // needed for vtkSmartPointer

VTK_ABI_NAMESPACE_BEGIN
class QVTKInteractor;
class QVTKInteractorAdapter;
class QVTKRenderWindowAdapter;
class vtkGenericOpenGLRenderWindow;

class VTKGUISUPPORTQT_EXPORT QVTKOpenGLNativeWidget : public QOpenGLWidget
{
  Q_OBJECT
  typedef QOpenGLWidget Superclass;

public:
  QVTKOpenGLNativeWidget(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());
  QVTKOpenGLNativeWidget(vtkGenericOpenGLRenderWindow* window, QWidget* parent = nullptr,
    Qt::WindowFlags f = Qt::WindowFlags());
  ~QVTKOpenGLNativeWidget() override;

  ///@{
  /**
   * Set a render window to use. It a render window was already set, it will be
   * finalized and all of its OpenGL resource released. If the \c win is
   * non-null and it has no interactor set, then a QVTKInteractor instance will
   * be created as set on the render window as the interactor.
   */
  void setRenderWindow(vtkGenericOpenGLRenderWindow* win);
  void setRenderWindow(vtkRenderWindow* win);
  ///@}

  /**
   * Returns the render window that is being shown in this widget.
   */
  vtkRenderWindow* renderWindow() const;

  /**
   * Get the QVTKInteractor that was either created by default or set by the user.
   */
  QVTKInteractor* interactor() const;

  /**
   * @copydoc QVTKRenderWindowAdapter::defaultFormat(bool)
   */
  static QSurfaceFormat defaultFormat(bool stereo_capable = false);

  ///@{
  /**
   * Enable or disable support for touch event processing. When enabled, this widget
   * will process Qt::TouchBegin/TouchUpdate/TouchEnd event, otherwise, these events
   * will be ignored. Default is true.
   */
  void setEnableTouchEventProcessing(bool enable);
  bool enableTouchEventProcesing() const { return this->EnableTouchEventProcessing; }
  ///@}

  ///@{
  /**
   * Enable or disable support for HiDPI displays. When enabled, this enabled
   * DPI scaling i.e. `vtkWindow::SetDPI` will be called with a DPI value scaled
   * by the device pixel ratio every time the widget is resized. The unscaled
   * DPI value can be specified by using `setUnscaledDPI`.
   */
  void setEnableHiDPI(bool enable);
  bool enableHiDPI() const { return this->EnableHiDPI; }
  ///@}

  ///@{
  /**
   * Set/Get unscaled DPI value. Defaults to 72, which is also the default value
   * in vtkWindow.
   */
  void setUnscaledDPI(int);
  int unscaledDPI() const { return this->UnscaledDPI; }
  ///@}

  ///@{
  /**
   * Set/Get a custom device pixel ratio to use to map Qt sizes to VTK (or
   * OpenGL) sizes. Thus, when the QWidget is resized, it called
   * `vtkRenderWindow::SetSize` on the internal vtkRenderWindow after
   * multiplying the QWidget's size by this scale factor.
   *
   * By default, this is set to 0. Which means that `devicePixelRatio` obtained
   * from Qt will be used. Set this to a number greater than 0 to override this
   * behaviour and use the custom scale factor instead.
   *
   * `effectiveDevicePixelRatio` can be used to obtain the device-pixel-ratio
   * that will be used given the value for customDevicePixelRatio.
   */
  void setCustomDevicePixelRatio(double cdpr);
  double customDevicePixelRatio() const { return this->CustomDevicePixelRatio; }
  double effectiveDevicePixelRatio() const;
  ///@}

  ///@{
  /**
   * Set/get the default cursor to use for this widget.
   */
  void setDefaultCursor(const QCursor& cursor);
  const QCursor& defaultCursor() const { return this->DefaultCursor; }
  ///@}

  ///@{
  /**
   * Convenience method by symmetry with QVTKOpenGLStereoWidget.
   * Internally just calls QWidget::setCursor / QWidget::cursor.
   */
  void setCursorCustom(const QCursor& cursor) { this->setCursor(cursor); }
  QCursor cursorCustom() const { return this->cursor(); }
  ///@}

protected Q_SLOTS:
  /**
   * Called as a response to `QOpenGLContext::aboutToBeDestroyed`. This may be
   * called anytime during the widget lifecycle. We need to release any OpenGL
   * resources allocated in VTK work in this method.
   */
  virtual void cleanupContext();

  void updateSize();

protected: // NOLINT(readability-redundant-access-specifiers)
  bool event(QEvent* evt) override;
  void initializeGL() override;
  void paintGL() override;

  vtkSmartPointer<vtkGenericOpenGLRenderWindow> RenderWindow;
  QScopedPointer<QVTKRenderWindowAdapter> RenderWindowAdapter;

private:
  Q_DISABLE_COPY(QVTKOpenGLNativeWidget);

  bool EnableTouchEventProcessing = true;
  bool EnableHiDPI;
  int UnscaledDPI;
  double CustomDevicePixelRatio;
  QCursor DefaultCursor;
};

VTK_ABI_NAMESPACE_END
#endif
