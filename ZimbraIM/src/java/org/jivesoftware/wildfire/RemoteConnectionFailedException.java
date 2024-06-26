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
package org.jivesoftware.wildfire;

/**
 * Thrown when something failed verifying the key of a Originating Server with an Authoritative
 * Server in a dialback operation.
 *
 * @author Gaston Dombiak
 */
public class RemoteConnectionFailedException extends Exception {

    public RemoteConnectionFailedException() {
        super();
    }

    public RemoteConnectionFailedException(String msg) {
        super(msg);
    }
}
