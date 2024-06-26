/*
 * ***** BEGIN LICENSE BLOCK *****
 * Zimbra Collaboration Suite Server
 * Copyright (C) 2006, 2007, 2009, 2010 Zimbra, Inc.
 * 
 * The contents of this file are subject to the Zimbra Public License
 * Version 1.3 ("License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 * http://www.zimbra.com/license.
 * 
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied.
 * ***** END LICENSE BLOCK *****
 */
package org.jivesoftware.wildfire.net;

import org.jivesoftware.util.LocaleUtils;
import org.jivesoftware.util.Log;
import org.jivesoftware.wildfire.Connection;
import org.jivesoftware.wildfire.ConnectionCloseListener;
import org.jivesoftware.wildfire.Session;

import java.util.HashMap;
import java.util.Map;

/**
 * Abstract implementation of the Connection interface that models abstract connections. Abstract
 * connections are connections that don't have a physical connection counterpart. Instead they
 * can be seen as conceptual or just 'abstract' connections.<p>
 *
 * Default values and common behavior of virtual connections are modeled in this class. Subclasses
 * should just need to specify how packets are delivered and what means closing the connection.
 *
 * @author Gaston Dombiak
 */
public abstract class VirtualConnection implements Connection {

    protected Session session;

    final private Map<ConnectionCloseListener, Object> listeners =
            new HashMap<ConnectionCloseListener, Object>();

    private boolean closed = false;

    public String getLanguage() {
        // Information not available. Return any value. This is not actually used.
        return null;
    }

    public int getMajorXMPPVersion() {
        // Information not available. Return any value. This is not actually used.
        return 0;
    }

    public int getMinorXMPPVersion() {
        // Information not available. Return any value. This is not actually used.
        return 0;
    }

    public boolean isClosed() {
        if (session == null) {
            return closed;
        }
        return session.getStatus() == Session.STATUS_CLOSED;
    }

    public Connection.CompressionPolicy getCompressionPolicy() {
        // Return null since compression is not used for virtual connections
        return null;
    }

    public Connection.TLSPolicy getTlsPolicy() {
        // Return null since TLS is not used for virtual connections
        return null;
    }

    public boolean isCompressed() {
        // Return false since compression is not used for virtual connections
        return false;
    }

    public boolean isFlashClient() {
        // Return false since flash clients is not used for virtual connections
        return false;
    }

    public boolean isSecure() {
        // Return false since TLS is not used for virtual connections
        return false;
    }

    public boolean validate() {
        // Return true since the virtual connection is valid until it no longer exists
        return true;
    }

    public void init(Session session) {
        this.session = session;
    }

    /**
     * Closes the session, the virtual connection and notifies listeners that the connection
     * has been closed.
     */
    public void close() {
        boolean wasClosed = false;
        synchronized (this) {
            if (!isClosed()) {
                try {
                    if (session != null) {
                        session.setStatus(Session.STATUS_CLOSED);
                    }
                    closeVirtualConnection();
                    closed = true;
                }
                catch (Exception e) {
                    Log.error(LocaleUtils.getLocalizedString("admin.error.close")
                            + "\n" + this.toString(), e);
                }
                wasClosed = true;
            }
        }
        if (wasClosed) {
            notifyCloseListeners();
        }
    }

    public Object registerCloseListener(ConnectionCloseListener listener, Object handbackMessage) {
        Object status = null;
        if (isClosed()) {
            listener.onConnectionClose(handbackMessage);
        }
        else {
            status = listeners.put(listener, handbackMessage);
        }
        return status;
    }

    public Object removeCloseListener(ConnectionCloseListener listener) {
        return listeners.remove(listener);
    }

    /**
     * Notifies all close listeners that the connection has been closed.
     */
    private void notifyCloseListeners() {
        synchronized (listeners) {
            for (ConnectionCloseListener listener : listeners.keySet()) {
                try {
                    listener.onConnectionClose(listeners.get(listener));
                }
                catch (Exception e) {
                    Log.error("Error notifying listener: " + listener, e);
                }
            }
        }
    }

    /**
     * Closes the virtual connection. Subsclasses should indicate what closing a virtual
     * connection means. At this point the session has a CLOSED state.
     */
    public abstract void closeVirtualConnection();
}
