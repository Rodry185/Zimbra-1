/*
 * ***** BEGIN LICENSE BLOCK *****
 * Zimbra Collaboration Suite Server
 * Copyright (C) 2007, 2008, 2009, 2010 Zimbra, Inc.
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
package com.zimbra.cs.offline.jsp;

import java.util.HashMap;
import java.util.Map;

import com.zimbra.common.service.ServiceException;
import com.zimbra.common.soap.SoapFaultException;
import com.zimbra.cs.account.Account;
import com.zimbra.cs.account.Provisioning;
import com.zimbra.cs.account.DataSource;
import com.zimbra.cs.offline.common.OfflineConstants;

public class ZmailBean extends MailBean {
    public ZmailBean() {
        port = "443";
        connectionType = DataSource.ConnectionType.ssl;
        syncFreqSecs = 0;
        type = "zimbra";
    }

    @Override
    protected void reload() {
        Account account = null;
        try {
            account = JspProvStub.getInstance().getOfflineAccount(accountId);
        } catch (ServiceException e) {
            setError(e.getMessage());
            return;
        }
        accountFlavor = account.getAttr(OfflineConstants.A_offlineAccountFlavor);
        accountName = account.getAttr(Provisioning.A_zimbraPrefLabel);
        accountName = accountName != null ? accountName :
            account.getAttr(OfflineConstants.A_offlineAccountName);
        email = account.getName();
        password = JspConstants.MASKED_PASSWORD;
        host = account.getAttr(JspConstants.OFFLINE_REMOTE_HOST);
        port = account.getAttr(JspConstants.OFFLINE_REMOTE_PORT);
        boolean ssl = account.getBooleanAttr(JspConstants.OFFLINE_REMOTE_SSL, false);
        connectionType = ssl ? DataSource.ConnectionType.ssl : DataSource.ConnectionType.cleartext;
        syncFreqSecs = account.getTimeIntervalSecs(OfflineConstants.A_offlineSyncFreq, 0);
        isDebugTraceEnabled = account.getBooleanAttr(OfflineConstants.A_offlineEnableTrace, false);
    }

    @Override
    protected void doRequest() {
        if (verb == null || !isAllOK())
            return;
        try {
            Map<String, Object> attrs = new HashMap<String, Object>();

            if (verb.isAdd() || verb.isModify()) {
                if (isEmpty(accountName))
                    addInvalid("accountName");
                if (isEmpty(accountFlavor))
                    addInvalid("flavor");
                if (!isValidEmail(email))
                    addInvalid("email");
                if (isEmpty(password))
                    addInvalid("password");
                if (!isValidHost(host))
                    addInvalid("host");
                if (!isEmpty(port) && !isValidPort(port))
                    addInvalid("port");
                if (isAllOK()) {
                    attrs.put(OfflineConstants.A_offlineAccountSetup, Provisioning.TRUE);

                    attrs.put(Provisioning.A_zimbraPrefLabel, accountName);
                    attrs.put(OfflineConstants.A_offlineRemoteServerUri,
                        getRemoteServerUri());
                    attrs.put(OfflineConstants.A_offlineSyncFreq,
                        Long.toString(syncFreqSecs));
                    attrs.put(OfflineConstants.A_offlineEnableTrace,
                        isDebugTraceEnabled ? Provisioning.TRUE : Provisioning.FALSE);
                    if (!password.equals(JspConstants.MASKED_PASSWORD))
                        attrs.put(OfflineConstants.A_offlineRemotePassword, password);
                    if (sslCertAlias != null && sslCertAlias.length() > 0)
                        attrs.put(OfflineConstants.A_offlineSslCertAlias, sslCertAlias);
                    attrs.put(JspConstants.OFFLINE_REMOTE_HOST, host);
                    attrs.put(JspConstants.OFFLINE_REMOTE_PORT, port);
                    attrs.put(JspConstants.OFFLINE_REMOTE_SSL, isSsl() ? Provisioning.TRUE : Provisioning.FALSE);
                }
            }

            JspProvStub stub = JspProvStub.getInstance();
            if (isAllOK()) {                
                attrs.put(OfflineConstants.A_offlineAccountFlavor, accountFlavor);
                if (verb.isAdd()) {
                    stub.createOfflineAccount(accountName, email, attrs);
                } else {
                    if (isEmpty(accountId)) {
                        setError(getMessage("AccountIdMissing"));
                    } else if (verb.isModify()) {
                        stub.modifyOfflineAccount(accountId, attrs);
                    } else if (verb.isReset()) {
                        stub.resetOfflineAccount(accountId);
                    } else if (verb.isDelete()) {
                        stub.deleteOfflineAccount(accountId);
                    } else if (verb.isReindex()) {
                        stub.reIndex(accountId);
                    } else {
                        setError(getMessage("UnknownAct"));
                    }
                }
            }
        } catch (SoapFaultException e) {
            if (!(verb != null && verb.isDelete() &&
                e.getCode().equals("account.NO_SUCH_ACCOUNT")))
                setExceptionError(e);
        } catch (Exception t) {
            setError(t.getLocalizedMessage() == null ? t.toString() : t.getLocalizedMessage());
        }
    }

    public boolean isDefaultPort() {
        if (isEmpty(port))
            return true;
        int iPort = Integer.parseInt(port);
        return (isSsl() && iPort == 443) || (!isSsl() && iPort == 80);
    }

    private String getRemoteServerUri() {
        return (isSsl() ? "https://" : "http://") + host +
        (isDefaultPort() ? "" : ":" + port);
    }

    public boolean isSmtpConfigSupported() {
        return false;
    }

    public boolean isUsernameRequired() {
        return false;
    }
    
    @Override
    public void setPassword(String input) {
        this.password = input;
    }
}

