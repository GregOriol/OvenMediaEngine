//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "https_server.h"

#include "../http_private.h"

// Reference: https://wiki.mozilla.org/Security/Server_Side_TLS

// Modern compatibility
#define HTTP_MODERN_COMPATIBILITY "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA256"
// Intermediate compatibility
#define HTTP_INTERMEDIATE_COMPATIBILITY "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA:ECDHE-RSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-RSA-AES256-SHA256:DHE-RSA-AES256-SHA:ECDHE-ECDSA-DES-CBC3-SHA:ECDHE-RSA-DES-CBC3-SHA:EDH-RSA-DES-CBC3-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA256:AES256-SHA256:AES128-SHA:AES256-SHA:DES-CBC3-SHA:!DSS"
// Backward compatibility
#define HTTP_BACKWARD_COMPATIBILITY "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:DHE-RSA-AES256-SHA:ECDHE-RSA-DES-CBC3-SHA:ECDHE-ECDSA-DES-CBC3-SHA:EDH-RSA-DES-CBC3-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA256:AES256-SHA256:AES128-SHA:AES256-SHA:AES:DES-CBC3-SHA:HIGH:SEED:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!RSAPSK:!aDH:!aECDH:!EDH-DSS-DES-CBC3-SHA:!KRB5-DES-CBC3-SHA:!SRP"
// Fastest suite only, which is still considered `secure`.
#define HTTP_FAST_NOT_VERY_SECURE "AES128-SHA"

namespace http
{
	namespace svr
	{
		std::shared_ptr<const ov::Error> HttpsServer::AppendCertificate(const std::shared_ptr<const info::Certificate> &certificate)
		{
			if (certificate == nullptr)
			{
				return ov::OpensslError::CreateError("Certificate is nullptr");
			}

			ov::TlsContextCallback tls_context_callback = {
				.create_callback = nullptr,
				.verify_callback = nullptr,
				std::bind(&HttpsServer::HandleSniCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)};

			std::shared_ptr<const ov::Error> error;
			auto tls_context = ov::TlsContext::CreateServerContext(
				ov::TlsMethod::Tls, certificate->GetCertificatePair(),
				HTTP_FAST_NOT_VERY_SECURE, &tls_context_callback,
				&error);

			if (tls_context == nullptr)
			{
				return error;
			}

			std::lock_guard lock_guard(_https_certificate_list_mutex);

			logtd("Appending a certificate for host: %s", certificate->ToString().CStr());

			_https_certificate_list.emplace_back(std::make_shared<HttpsCertificate>(certificate, tls_context));

			return nullptr;
		}

		std::shared_ptr<const ov::Error> HttpsServer::AppendCertificateList(const std::vector<std::shared_ptr<const info::Certificate>> &certificate_list)
		{
			for (auto &certificate : certificate_list)
			{
				auto error = AppendCertificate(certificate);

				if (error != nullptr)
				{
					return error;
				}
			}

			return nullptr;
		}

		void HttpsServer::OnConnected(const std::shared_ptr<ov::Socket> &remote)
		{
			std::shared_ptr<HttpsCertificate> https_certificate;

			{
				std::lock_guard lock_guard(_https_certificate_list_mutex);
				if (_https_certificate_list.empty())
				{
					logte("Could not handle connection event: there is no certificate");
					return;
				}

				https_certificate = _https_certificate_list.front();
			}

			auto client = ProcessConnect(remote);

			if (client == nullptr)
			{
				return;
			}

			auto tls_data = std::make_shared<ov::TlsServerData>(https_certificate->tls_context, true);

			tls_data->SetWriteCallback([remote](const void *data, size_t length) -> ssize_t {
				return remote->Send(data, length) ? length : -1L;
			});

			client->GetRequest()->SetTlsData(tls_data);
			client->GetResponse()->SetTlsData(tls_data);
		}

		void HttpsServer::OnDataReceived(const std::shared_ptr<ov::Socket> &remote, const ov::SocketAddress &address, const std::shared_ptr<const ov::Data> &data)
		{
			auto client = FindClient(remote);

			if (client == nullptr)
			{
				// This can be called in situations where the client closes the connection from the server at the same time as the data is sent
				return;
			}

			auto request = client->GetRequest();
			auto tls_data = request->GetTlsData();

			if (tls_data != nullptr)
			{
				std::shared_ptr<const ov::Data> plain_data;

				if (tls_data->Decrypt(data, &plain_data))
				{
					if ((plain_data != nullptr) && (plain_data->GetLength() > 0))
					{
						// plain_data is HTTP data

						// Use the decrypted data
						client->ProcessData(plain_data);
					}
					else
					{
						// Need more data to decrypt the data
					}

					return;
				}

				// Error
				logtd("Could not decrypt data");
			}
			else
			{
				OV_ASSERT(false, "TlsServerData must not be null");
			}

			client->GetResponse()->Close();
		}

		bool HttpsServer::HandleSniCallback(ov::TlsContext *tls_context, SSL *ssl, const ov::String &server_name)
		{
			std::shared_ptr<HttpsCertificate> https_certificate;

			{
				std::lock_guard lock_guard(_https_certificate_list_mutex);
				for (auto &https_cert : _https_certificate_list)
				{
					if (https_cert->certificate->IsCertificateForHost(server_name))
					{
						https_certificate = https_cert;
						break;
					}
				}
			}

			if (https_certificate == nullptr)
			{
				logtw("Could not find certificate for server_name: %s", server_name.CStr());
				return false;
			}

			auto &certificate = https_certificate->certificate;

			if (https_certificate->tls_context->UseSslContext(ssl))
			{
				logtd("Certification found for server_name: %s, certificate: %s", server_name.CStr(), certificate->ToString().CStr());
				return true;
			}

			logte("Could not set SSL context %s to %p", certificate->ToString().CStr(), ssl);

			return false;
		}
	}  // namespace svr
}  // namespace http
