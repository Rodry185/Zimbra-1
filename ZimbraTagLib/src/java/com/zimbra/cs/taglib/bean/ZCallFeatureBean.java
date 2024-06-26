/*
 * ***** BEGIN LICENSE BLOCK *****
 * Zimbra Collaboration Suite Server
 * Copyright (C) 2007, 2009, 2010 Zimbra, Inc.
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
package com.zimbra.cs.taglib.bean;

import com.zimbra.cs.zclient.ZCallFeature;

public class ZCallFeatureBean {
    private ZCallFeature mFeature;

    public ZCallFeatureBean(ZCallFeature feature) {
        mFeature = feature;
    }

    public void setIsActive(boolean isActive) {
        mFeature.setIsActive(isActive);
    }

    public boolean getIsActive() {
		return mFeature.getIsActive();
	}

    public boolean getIsSubscribed() {
		return mFeature.getIsSubscribed();
	}

    public String getName() {
		return mFeature.getName();
	}

    protected ZCallFeature getFeature() {
        return mFeature;
    }
}
