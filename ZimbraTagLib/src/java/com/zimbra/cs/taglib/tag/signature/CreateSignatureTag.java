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
package com.zimbra.cs.taglib.tag.signature;

import com.zimbra.common.service.ServiceException;
import com.zimbra.cs.taglib.tag.ZimbraSimpleTag;
import com.zimbra.cs.zclient.ZSignature;

import javax.servlet.jsp.JspException;
import javax.servlet.jsp.JspTagException;
import javax.servlet.jsp.PageContext;
import java.io.IOException;

public class CreateSignatureTag extends ZimbraSimpleTag {

    private String mName;
    private String mVar;
    private String mValue;
    private String mType = "text/plain";
    
    public void setName(String name) { mName = name; }
    public void setValue(String value) { mValue = value; }
    public void setVar(String var) { mVar = var; }
    public void setType(String type) { mType = type; }
    
    public void doTag() throws JspException, IOException {
        try {

            ZSignature sig = new ZSignature(mName, mValue);
            sig.setType(mType);
            
            String id = getMailbox().createSignature(sig);
            getJspContext().setAttribute(mVar, id, PageContext.PAGE_SCOPE);
        } catch (ServiceException e) {
            throw new JspTagException(e);
        }
    }
}
